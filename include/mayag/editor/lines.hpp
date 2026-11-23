#pragma once
// mayag::editor Lines — a document's lines in shared, immutable chunks.
//
//   Lines ls;                        // one empty line
//   ls.assign(std::move(all));       // a whole new text
//   ls.replace(at, n, std::move(v)); // lines [at, at+n) become v
//   ls[i], ls.size(), for (auto& l : ls)
//
// Why not std::vector<std::string>: the editor's state lives in an app model
// that is a VALUE -- the runtime copies it before each update (so a throwing
// handler can be rolled back) and views keep their own. A 100k-line file
// copied and freed per keystroke was ~45% of the keystroke. Here a copy is
// the chunk pointers (~400 for 100k lines); an edit rebuilds the one or two
// chunks it touches (<= 512 short strings) and shares the rest. Chunks are
// const and never written after they are built, so sharing them is safe
// anywhere a copy goes.

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace mayag::editor {

class Lines {
  public:
    using Chunk = std::vector<std::string>;
    static constexpr std::size_t kTarget = 256;   ///< lines per chunk when built
    static constexpr std::size_t kMax = 512;      ///< a chunk past this is split

    Lines() { assign({std::string{}}); }
    Lines(std::initializer_list<std::string> l) { assign(std::vector<std::string>(l)); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] const std::string& operator[](std::size_t i) const {
        const std::size_t k = chunk_of(i);
        return (*chunks_[k])[i - start_[k]];
    }
    [[nodiscard]] const std::string& front() const { return (*this)[0]; }
    [[nodiscard]] const std::string& back() const { return (*this)[size_ - 1]; }

    /// A whole new text (at least one line is kept: an empty document has one).
    void assign(std::vector<std::string> all) {
        if (all.empty()) all.emplace_back();
        chunks_.clear();
        start_.clear();
        size_ = all.size();
        for (std::size_t i = 0; i < all.size(); i += kTarget) {
            const std::size_t e = std::min(all.size(), i + kTarget);
            start_.push_back(i);
            chunks_.push_back(std::make_shared<const Chunk>(std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(i)),
                                                            std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(e))));
        }
    }

    /// Lines [at, at+count) become `with` (count may be 0 to insert, `with`
    /// empty to delete). Only the chunks holding the range are rebuilt.
    void replace(std::size_t at, std::size_t count, std::vector<std::string> with) {
        at = std::min(at, size_);
        count = std::min(count, size_ - at);
        // the chunks the range touches (an insert at the very end joins the last)
        const std::size_t k0 = chunk_of(at == size_ ? size_ - 1 : at);
        const std::size_t k1 = count == 0 ? k0 : chunk_of(at + count - 1);
        Chunk merged;
        const Chunk& first = *chunks_[k0];
        const Chunk& last = *chunks_[k1];
        const std::size_t head = std::min(at - start_[k0], first.size());
        const std::size_t tail_from = count == 0 ? head : at + count - start_[k1];
        merged.reserve(head + with.size() + (last.size() - std::min(tail_from, last.size())));
        merged.insert(merged.end(), first.begin(), first.begin() + static_cast<std::ptrdiff_t>(head));
        merged.insert(merged.end(), std::make_move_iterator(with.begin()), std::make_move_iterator(with.end()));
        if (tail_from < last.size()) merged.insert(merged.end(), last.begin() + static_cast<std::ptrdiff_t>(tail_from), last.end());
        // the merged run back as chunks of <= kMax
        std::vector<std::shared_ptr<const Chunk>> pieces;
        if (merged.size() <= kMax) {
            if (!merged.empty()) pieces.push_back(std::make_shared<const Chunk>(std::move(merged)));
        } else {
            const std::size_t n = (merged.size() + kTarget - 1) / kTarget, per = (merged.size() + n - 1) / n;
            for (std::size_t i = 0; i < merged.size(); i += per) {
                const std::size_t e = std::min(merged.size(), i + per);
                pieces.push_back(std::make_shared<const Chunk>(std::make_move_iterator(merged.begin() + static_cast<std::ptrdiff_t>(i)),
                                                               std::make_move_iterator(merged.begin() + static_cast<std::ptrdiff_t>(e))));
            }
        }
        const auto K0 = static_cast<std::ptrdiff_t>(k0), K1 = static_cast<std::ptrdiff_t>(k1);
        chunks_.erase(chunks_.begin() + K0, chunks_.begin() + K1 + 1);
        chunks_.insert(chunks_.begin() + K0, pieces.begin(), pieces.end());
        size_ = size_ - count + with.size();
        if (chunks_.empty()) { assign({}); return; }   // everything deleted: one empty line
        // starts from k0 on
        start_.resize(chunks_.size());
        std::size_t s = k0 == 0 ? 0 : start_[k0 - 1] + chunks_[k0 - 1]->size();
        for (std::size_t k = k0; k < chunks_.size(); ++k) { start_[k] = s; s += chunks_[k]->size(); }
    }

    [[nodiscard]] std::vector<std::string> to_vector() const {
        std::vector<std::string> v;
        v.reserve(size_);
        for (const auto& c : chunks_) v.insert(v.end(), c->begin(), c->end());
        return v;
    }
    /// How many chunks (for tests: edits must stay local).
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }
    /// Whether line `i`'s chunk is the very one `o` holds (shared, not copied).
    [[nodiscard]] bool shares_chunk_with(const Lines& o, std::size_t i) const {
        return i < size_ && i < o.size_ && chunks_[chunk_of(i)] == o.chunks_[o.chunk_of(i)];
    }

    class const_iterator {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::string;
        using difference_type = std::ptrdiff_t;
        using pointer = const std::string*;
        using reference = const std::string&;
        const_iterator() = default;
        const_iterator(const Lines* l, std::size_t k, std::size_t j) : l_(l), k_(k), j_(j) {}
        reference operator*() const { return (*l_->chunks_[k_])[j_]; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() {
            if (++j_ >= l_->chunks_[k_]->size()) { ++k_; j_ = 0; }
            return *this;
        }
        const_iterator operator++(int) { auto t = *this; ++*this; return t; }
        friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.k_ == b.k_ && a.j_ == b.j_; }
      private:
        const Lines* l_ = nullptr;
        std::size_t k_ = 0, j_ = 0;
    };
    [[nodiscard]] const_iterator begin() const { return {this, 0, 0}; }
    [[nodiscard]] const_iterator end() const { return {this, chunks_.size(), 0}; }

  private:
    std::vector<std::shared_ptr<const Chunk>> chunks_;
    std::vector<std::size_t> start_;   ///< start_[k]: the first line of chunk k
    std::size_t size_ = 0;

    [[nodiscard]] std::size_t chunk_of(std::size_t i) const {
        const auto it = std::upper_bound(start_.begin(), start_.end(), i);
        return static_cast<std::size_t>(it - start_.begin()) - 1;
    }
};

}  // namespace mayag::editor
