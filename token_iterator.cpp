#include <cassert>
#include <clang-c/Index.h>
#include <gsl/gsl>

// clang_getTokenLocation() will sometimes return
// a CXSourceLocation that points to the middle of the entity.
// Use the start/end positions of clang_getTokenExtent() instead because they are better behaved.
class cursor_location {
    CXSourceLocation m_loc;

public:
    static constexpr int begin = 0;
    static constexpr int end = 1;

    cursor_location(const CXCursor &cursor, int pos = begin) {
        auto extent = clang_getCursorExtent(cursor);
        m_loc = (pos == begin) ? clang_getRangeStart(extent) : clang_getRangeEnd(extent);
    }

    const CXSourceLocation &get() const noexcept {
        return m_loc;
    }
};

class token_iterator {
    struct token_deleter {
        CXTranslationUnit tu;

        token_deleter() noexcept 
        :tu{ nullptr }
        {};

        token_deleter(CXTranslationUnit t) noexcept
        :tu{ t }
        {}

        void operator()(CXToken* tok) const {
            if (tok) {
                assert(tu);
                clang_disposeTokens(tu, tok, 1);
            }
        }
    };

    using unique_token = std::unique_ptr<CXToken, token_deleter>;
    unique_token m_tok;

    unique_token clone() const {
        if (!m_tok) return unique_token{};
        return unique_token{ clang_getToken(tu(), clang_getRangeStart(extent())), tu() };
    }

    CXTranslationUnit tu() const {
        assert(m_tok);
        return m_tok.get_deleter().tu;
    }

    CXSourceRange extent() const {
        assert(m_tok);
        return clang_getTokenExtent(tu(), *m_tok);
    }

    // NOTE: the start location is either the end location of the previous token 
    //       or the location of the first character of the current token
    // 
    // The end location is consistent regardless of which start location scheme is used

public:
    using difference_type = std::ptrdiff_t;
    using value_type = CXToken;
    using pointer = const CXToken*;
    using reference = const CXToken&;
    using iterator_category = std::forward_iterator_tag;

    // The end sentinel
    token_iterator() = default;

    token_iterator(CXTranslationUnit tu, const CURSOR_LOCATION &loc) 
    :m_tok{ clang_getToken(tu, loc.get()), tu }
    {}

    token_iterator(token_iterator &&other) noexcept
    :m_tok{ std::move(other.m_tok) }
    {}

    token_iterator(const token_iterator &other)
    :m_tok{ other.clone() } 
    {}

    token_iterator &operator=(token_iterator &&other) noexcept {
        m_tok = std::move(other.m_tok);
        return *this;
    }

    token_iterator &operator=(const token_iterator &other) {
        m_tok = other.clone();
        return *this;
    }

    token_iterator &operator++() {
        assert(m_tok);
        auto prev_extent = extent();
        auto prev_end = clang_getRangeEnd(prev_extent);

        m_tok.reset(clang_getToken(tu(), prev_end));
        return *this;
    }

    token_iterator operator++(int) {
        auto temp = *this;
        operator++();
        return temp;
    }

    // WARNING: Does not work across files, even if
    // they are contained within the same TU!
    token_iterator &operator--() {
        // TODO: Refactor into separate reverse iterator class
        //       Make forward and reverse iterators convertible between one another.
        //       The "end" sentinels of each class may not be converted to the other type.
        assert(m_tok);

        // Record current location information
        const auto curr_extent = extent();
        const auto curr_start = clang_getRangeStart(curr_extent);
        const auto curr_end = clang_getRangeEnd(curr_extent);

        // Retrieve file handle and current offset
        CXFile file = nullptr;
        unsigned int offset = 0;
        clang_getSpellingLocation(curr_start, &file, nullptr, nullptr, &offset);
        assert(file);
        assert(offset > 0); // TODO: convert to past-the-end sentinel in reverse iterator class.

        // Retrieve file buffer that we can offset into
        std::size_t file_size = 0;
        const char* file_buffer = clang_getFileContents(tu(), file, &file_size);
        assert(file_buffer);
        assert(offset < file_size);

        auto search_span = gsl::make_span(file_buffer, file_size);

        // Step 1: Decrement offset until the lexer binds the end to a different position.
        // That's when we know a new token has been found!
        unique_token candidate_tok{ nullptr, tu() };
        CXSourceLocation candidate_end;

        while (true) {
            assert(offset > 0);
            --offset;

            if (std::isspace(search_span[offset])) {
                // Fast path.
                continue;
            }
            else {
                auto candidate_loc = clang_getLocationForOffset(tu(), file, offset);
                candidate_tok.reset(clang_getToken(tu(), candidate_loc));

                if (candidate_tok) {
                    candidate_end = clang_getRangeEnd(clang_getTokenExtent(tu(), *candidate_tok));
                    if (!clang_equalLocations(candidate_end, curr_end)) {
                        break;
                    }
                }

                // In most (all?) cases, this branch will only be evaluated once
                // before a valid candidate is found.
            }
        }

        // Step 2: Ok, now to find the beginning of this token        
        search_span = search_span.first(offset);
        // Note that we dropped the offset-th character because we already know for sure it sits within
        // the bounds of the candidate token

        // Reduce the number of CXTokens created (and all the associated overhead) by performing binary search
        // First, identify the bounds of the string (non-whitespace characters)
        auto str_begin = std::find_if(search_span.rbegin(), search_span.rend(), 
                                      [](char c){ return std::isspace(c); });
        auto str_length = std::distance(search_span.rbegin(), str_begin);
        search_span = search_span.last(str_length);

        if (!search_span.empty()) {
            // We would love to use std::lower_bound, but we want to avoid unnecessary (re)-allocations
            // Caching is an option, but inelegant
            // You can't deny a span-based implementation is not nice too!

            // Helper. Returns false if the given pos goes past the candidate token
            // Returns true if it's within the candidate token bounds, and replace the candidate token with
            // the CXToken from this pos.
            auto consider_next_candidate = [&](const char* pos) {
                auto offset = gsl::narrow<unsigned int>(pos - file_buffer);
                auto next_candidate_loc = clang_getLocationForOffset(tu(), file, offset);

                unique_token next_candidate_tok{ clang_getToken(tu(), next_candidate_loc), tu() };
                if (next_candidate_tok) {
                    auto next_candidate_end = clang_getRangeEnd(clang_getTokenExtent(tu(), *next_candidate_tok));

                    if (!clang_equalLocations(next_candidate_end, candidate_end)) {
                        return false;
                    }

                    candidate_tok = std::move(next_candidate_tok);
                    return true;
                }

                return false;
            };

            if (consider_next_candidate(search_span.data())) {
                // Heuristic - consider the first character of the string first
                // Nothing to do - consider_next_candidate() has side effects.
            }
            else {
                // Drop the first character because we already checked that and it didn't work out
                search_span = search_span.subspan(1);

                // We would love to use std::lower_bound, but we want to avoid unnecessary (re)-allocations
                // Caching is an option, but inelegant
                // You can't deny a span-based implementation is not nice too!
             
                while (!search_span.empty()) {
                    auto mid_pos = search_span.size() / 2;

                    if (consider_next_candidate(&search_span[mid_pos])) {
                        // Still in bounds. Continue to the left.
                        search_span = search_span.first(mid_pos);
                    }
                    else {
                        // Overshot. Backtrack to the right.
                        search_span = search_span.subspan(mid_pos + 1); 
                        // +1 because we already considered mid_pos
                    }
                }
            }
        }
        // else: one character token

        m_tok = std::move(candidate_tok);
        return *this;
    }

    token_iterator operator--(int) {
        auto temp = *this;
        operator--();
        return temp;
    }

    inline reference operator*() const {
        assert(m_tok);
        return *m_tok;
    }

    inline pointer operator->() const {
        assert(m_tok);
        return m_tok.get();
    }

    bool operator==(const token_iterator &other) const {
        if (bool(m_tok) != bool(other.m_tok)) return false;
        if (!m_tok) return true;

        return tu() == other.tu() && 
               clang_equalLocations(clang_getRangeEnd(extent()), clang_getRangeEnd(other.extent()));
    }

    bool operator!=(const token_iterator &other) const {
        return !operator==(other);
    }

    bool is_end_sentinel() const noexcept { return !m_tok; }
    operator bool() const noexcept { return !is_end_sentinel(); }

    friend void swap(token_iterator &lhs, token_iterator &rhs) noexcept {
        std::swap(lhs.m_tok, rhs.m_tok);
    }
};
