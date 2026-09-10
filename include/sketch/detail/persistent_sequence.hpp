#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace sketch::detail {

// An immutable, value-semantic sequence optimized for append-heavy history.
// The current tail is copied when it is partial, so an append copies at most
// ChunkSize - 1 old values. Completed predecessor chunks are shared. A logical
// front index makes queue-style removal allocation-free without rebuilding the
// predecessor chain.
template <typename T, std::size_t ChunkSize = 32>
class PersistentSequence final {
    static_assert(ChunkSize > 0);

    struct Node final {
        std::atomic_size_t references{1};
        Node* previous{};
        std::size_t physical_size{};
        std::size_t count{};
        std::array<std::optional<T>, ChunkSize> values;

        template <typename U>
        Node(Node* predecessor, U&& value)
            : previous(predecessor),
              physical_size((predecessor == nullptr ? 0 : predecessor->physical_size) + 1),
              count(1) {
            values[0].emplace(std::forward<U>(value));
            retain(previous);
            allocation_count_.fetch_add(1, std::memory_order_relaxed);
            live_count_.fetch_add(1, std::memory_order_relaxed);
        }

        template <typename U>
        Node(const Node& old_tail, U&& value, bool replace)
            : previous(old_tail.previous),
              physical_size(old_tail.physical_size + (replace ? 0 : 1)),
              count(old_tail.count + (replace ? 0 : 1)) {
            const auto copied = old_tail.count - (replace ? 1 : 0);
            for (std::size_t index = 0; index < copied; ++index) {
                values[index].emplace(*old_tail.values[index]);
            }
            values[copied].emplace(std::forward<U>(value));
            retain(previous);
            allocation_count_.fetch_add(1, std::memory_order_relaxed);
            live_count_.fetch_add(1, std::memory_order_relaxed);
        }

        ~Node() {
            live_count_.fetch_sub(1, std::memory_order_relaxed);
        }

        static void retain(Node* node) noexcept {
            if (node != nullptr) node->references.fetch_add(1, std::memory_order_relaxed);
        }

        // Nodes use a raw predecessor plus an intrusive count so releasing the
        // last root walks a long predecessor chain iteratively. A shared_ptr
        // predecessor would recursively destroy thousands of chunks.
        static void release(Node* node) noexcept {
            while (node != nullptr) {
                if (node->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
                auto* predecessor = node->previous;
                node->previous = nullptr;
                delete node;
                node = predecessor;
            }
        }
    };

public:
    PersistentSequence() noexcept = default;

    PersistentSequence(const PersistentSequence& other) noexcept
        : root_(other.root_), first_index_(other.first_index_) {
        Node::retain(root_);
    }

    PersistentSequence& operator=(const PersistentSequence& other) noexcept {
        if (this == &other) return *this;
        PersistentSequence candidate(other);
        swap(candidate);
        return *this;
    }

    PersistentSequence(PersistentSequence&& other) noexcept
        : root_(std::exchange(other.root_, nullptr)),
          first_index_(std::exchange(other.first_index_, 0)) {}

    PersistentSequence& operator=(PersistentSequence&& other) noexcept {
        if (this == &other) return *this;
        PersistentSequence candidate(std::move(other));
        swap(candidate);
        return *this;
    }

    ~PersistentSequence() {
        Node::release(root_);
    }

    void swap(PersistentSequence& other) noexcept {
        std::swap(root_, other.root_);
        std::swap(first_index_, other.first_index_);
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return root_ == nullptr ? 0 : root_->physical_size - first_index_;
    }

    [[nodiscard]] const T& back() const {
        if (empty()) throw std::out_of_range("persistent sequence is empty");
        return *root_->values[root_->count - 1];
    }

    [[nodiscard]] const T& front() const {
        if (empty()) throw std::out_of_range("persistent sequence is empty");
        auto* node = root_;
        while (node != nullptr) {
            const auto predecessor_size =
                node->previous == nullptr ? 0 : node->previous->physical_size;
            if (first_index_ >= predecessor_size) {
                return *node->values[first_index_ - predecessor_size];
            }
            node = node->previous;
        }
        throw std::out_of_range("persistent sequence front is inconsistent");
    }

    void push_back(const T& value) {
        append(value);
    }

    void push_back(T&& value) {
        append(std::move(value));
    }

    void replace_back(const T& value) {
        replace(value);
    }

    void replace_back(T&& value) {
        replace(std::move(value));
    }

    void pop_front() noexcept {
        if (empty()) return;
        ++first_index_;
        if (first_index_ == root_->physical_size) clear();
    }

    void clear() noexcept {
        auto* old_root = std::exchange(root_, nullptr);
        first_index_ = 0;
        Node::release(old_root);
    }

    template <typename Predicate>
    [[nodiscard]] const T* find_last_if(Predicate&& predicate) const {
        if (empty()) return nullptr;
        auto* node = root_;
        while (node != nullptr) {
            const auto predecessor_size =
                node->previous == nullptr ? 0 : node->previous->physical_size;
            for (std::size_t index = node->count; index > 0; --index) {
                const auto physical_index = predecessor_size + index - 1;
                if (physical_index < first_index_) return nullptr;
                const auto& value = *node->values[index - 1];
                if (predicate(value)) return &value;
            }
            node = node->previous;
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<T> materialize() const {
        std::vector<T> result;
        result.reserve(size());
        if (empty()) return result;

        std::vector<const Node*> chunks;
        chunks.reserve(chunk_count());
        for (auto* node = root_; node != nullptr; node = node->previous) {
            chunks.push_back(node);
        }
        std::size_t physical_index = 0;
        for (auto chunk = chunks.rbegin(); chunk != chunks.rend(); ++chunk) {
            for (std::size_t index = 0; index < (*chunk)->count; ++index, ++physical_index) {
                if (physical_index >= first_index_) {
                    result.push_back(*(*chunk)->values[index]);
                }
            }
        }
        return result;
    }

    [[nodiscard]] std::size_t chunk_count() const noexcept {
        std::size_t result = 0;
        for (auto* node = root_; node != nullptr; node = node->previous) ++result;
        return result;
    }

    [[nodiscard]] const void* root_identity() const noexcept {
        return root_;
    }

    // Only the changed tail is newly allocated by append/replace. Pointees
    // shared by T must be excluded by dynamic_bytes.
    template <typename DynamicBytes>
    [[nodiscard]] std::size_t tail_allocation_bytes(DynamicBytes&& dynamic_bytes) const {
        if (!root_) return 0;
        std::size_t result = sizeof(Node) + 32;
        for (std::size_t i = 0; i < root_->count; ++i) {
            const auto extra = dynamic_bytes(*root_->values[i]);
            if (extra > static_cast<std::size_t>(-1) - result)
                throw std::invalid_argument("persistent sequence byte accounting overflow");
            result += extra;
        }
        return result;
    }

    [[nodiscard]] bool same_version(const PersistentSequence& other) const noexcept {
        return root_ == other.root_ && first_index_ == other.first_index_;
    }

    [[nodiscard]] static std::size_t live_chunk_count() noexcept {
        return live_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static std::size_t chunk_allocation_count() noexcept {
        return allocation_count_.load(std::memory_order_relaxed);
    }

private:
    template <typename U>
    void append(U&& value) {
        Node* candidate = root_ == nullptr || root_->count == ChunkSize
                              ? new Node(root_, std::forward<U>(value))
                              : new Node(*root_, std::forward<U>(value), false);
        PersistentSequence publication(candidate, first_index_);
        swap(publication);
    }

    template <typename U>
    void replace(U&& value) {
        if (empty()) throw std::out_of_range("persistent sequence is empty");
        auto* candidate = new Node(*root_, std::forward<U>(value), true);
        PersistentSequence publication(candidate, first_index_);
        swap(publication);
    }

    PersistentSequence(Node* root, std::size_t first_index) noexcept
        : root_(root), first_index_(first_index) {}

    Node* root_{};
    std::size_t first_index_{};

    static inline std::atomic_size_t live_count_{};
    static inline std::atomic_size_t allocation_count_{};
};

template <typename T, std::size_t ChunkSize>
void swap(PersistentSequence<T, ChunkSize>& left,
          PersistentSequence<T, ChunkSize>& right) noexcept {
    left.swap(right);
}

}  // namespace sketch::detail
