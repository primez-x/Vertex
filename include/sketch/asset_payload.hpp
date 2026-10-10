#pragma once

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

std::string sha256_hex(std::span<const std::byte> bytes);

// Copies share owned bytes. All contiguous views are read-only; replacing the
// payload or assigning a byte publishes a new immutable buffer for this owner.
class AssetPayload {
public:
    using value_type = std::byte;
    using size_type = std::vector<value_type>::size_type;
    using difference_type = std::vector<value_type>::difference_type;
    using const_reference = const value_type&;
    using const_pointer = const value_type*;
    using pointer = const_pointer;
    using const_iterator = std::vector<value_type>::const_iterator;
    using iterator = const_iterator;

    // A proxy belongs to its container and index, not to a shared buffer. It
    // must not outlive that container. Replacing/moving the container does not
    // retarget an existing proxy to another owner; each access checks the index.
    class ByteReference {
    public:
        ByteReference(const ByteReference&) noexcept = default;
        operator value_type() const { return owner_->storage().at(index_); }

        ByteReference& operator=(value_type value) {
            owner_->replace_byte(index_, value);
            return *this;
        }
        ByteReference& operator=(const ByteReference& value) {
            return *this = static_cast<value_type>(value);
        }
        ByteReference& operator&=(value_type value) {
            return *this = static_cast<value_type>(*this) & value;
        }
        ByteReference& operator|=(value_type value) {
            return *this = static_cast<value_type>(*this) | value;
        }
        ByteReference& operator^=(value_type value) {
            return *this = static_cast<value_type>(*this) ^ value;
        }
        template <class Integer>
        ByteReference& operator<<=(Integer shift) {
            return *this = static_cast<value_type>(*this) << shift;
        }
        template <class Integer>
        ByteReference& operator>>=(Integer shift) {
            return *this = static_cast<value_type>(*this) >> shift;
        }

    private:
        friend class AssetPayload;
        ByteReference(AssetPayload& owner, size_type index) noexcept
            : owner_(&owner), index_(index) {}
        AssetPayload* owner_;
        size_type index_;
    };
    using reference = ByteReference;

    AssetPayload() noexcept = default;
    // Freeze a private copy, including rvalue input: a caller may still retain
    // a mutable pointer into a moved vector. Such aliases never reach this
    // immutable buffer or invalidate its cached proof.
    AssetPayload(std::vector<value_type> bytes) {
        if (!bytes.empty())
            bytes_ = std::make_shared<const Storage>(bytes);
    }
    AssetPayload(std::initializer_list<value_type> bytes)
        : AssetPayload(std::vector<value_type>(bytes)) {}
    AssetPayload(const AssetPayload&) noexcept = default;
    AssetPayload(AssetPayload&&) noexcept = default;
    AssetPayload& operator=(const AssetPayload&) noexcept = default;
    AssetPayload& operator=(AssetPayload&&) noexcept = default;

    AssetPayload& operator=(std::vector<value_type> bytes) {
        AssetPayload replacement(std::move(bytes));
        bytes_ = std::move(replacement.bytes_);
        return *this;
    }
    AssetPayload& operator=(std::initializer_list<value_type> bytes) {
        return *this = std::vector<value_type>(bytes);
    }
    template <std::input_iterator InputIterator>
    void assign(InputIterator first, InputIterator last) {
        // Empty pointer ranges need not perform null-pointer subtraction.
        if (first == last) {
            bytes_.reset();
            return;
        }
        *this = std::vector<value_type>(first, last);
    }
    void assign(std::vector<value_type> bytes) { *this = std::move(bytes); }
    void assign(std::initializer_list<value_type> bytes) { *this = bytes; }

    size_type size() const noexcept { return storage().size(); }
    bool empty() const noexcept { return storage().empty(); }
    const_pointer data() const noexcept { return storage().data(); }
    const_iterator begin() const noexcept { return storage().cbegin(); }
    const_iterator end() const noexcept { return storage().cend(); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    const_reference operator[](size_type index) const noexcept { return storage()[index]; }
    ByteReference operator[](size_type index) noexcept { return {*this, index}; }
    const_reference at(size_type index) const { return storage().at(index); }
    ByteReference at(size_type index) {
        (void)storage().at(index);
        return {*this, index};
    }
    const_reference front() const noexcept { return storage().front(); }
    ByteReference front() noexcept { return {*this, 0}; }
    const_reference back() const noexcept { return storage().back(); }
    ByteReference back() noexcept { return {*this, size() - 1}; }

    bool same_storage(const AssetPayload& other) const noexcept {
        return bytes_ == other.bytes_;
    }
    // The digest is computed from private immutable bytes, never from a caller's
    // declared hash. Copies reuse the same proof; every replacement clears it.
    std::string verified_sha256() const {
        if (!bytes_) return sha256_hex(std::span<const std::byte>{});
        std::call_once(bytes_->hash_once, [owned = bytes_] {
            owned->hash = sha256_hex(std::span<const std::byte>(owned->bytes));
        });
        return bytes_->hash;
    }
    friend bool operator==(const AssetPayload& left, const AssetPayload& right) {
        return left.same_storage(right) || left.storage() == right.storage();
    }
    friend bool operator==(const AssetPayload& left, const std::vector<value_type>& right) {
        return left.storage() == right;
    }
    friend bool operator==(const std::vector<value_type>& left, const AssetPayload& right) {
        return left == right.storage();
    }

private:
    struct Storage {
        explicit Storage(const std::vector<value_type>& value) : bytes(value) {}
        const std::vector<value_type> bytes;
        mutable std::once_flag hash_once;
        mutable std::string hash;
    };
    const std::vector<value_type>& storage() const noexcept {
        static const std::vector<value_type> empty;
        return bytes_ ? bytes_->bytes : empty;
    }
    void replace_byte(size_type index, value_type value) {
        if (storage().at(index) == value) return;
        auto replacement = storage();
        replacement[index] = value;
        *this = std::move(replacement);
    }

    std::shared_ptr<const Storage> bytes_;
};

} // namespace sketch
