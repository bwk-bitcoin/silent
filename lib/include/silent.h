#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#if __cplusplus >= 201703L
#include <string_view>
#endif
#if __cplusplus >= 202002L
#include <ranges>
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif // __clang__

namespace rust {
inline namespace cxxbridge1 {
// #include "rust/cxx.h"

#ifndef CXXBRIDGE1_PANIC
#define CXXBRIDGE1_PANIC
template <typename Exception>
void panic [[noreturn]] (const char *msg);
#endif // CXXBRIDGE1_PANIC

struct unsafe_bitcopy_t;

namespace {
template <typename T>
class impl;
} // namespace

template <typename T>
::std::size_t size_of();
template <typename T>
::std::size_t align_of();

#ifndef CXXBRIDGE1_RUST_STRING
#define CXXBRIDGE1_RUST_STRING
class String final {
public:
  String() noexcept;
  String(const String &) noexcept;
  String(String &&) noexcept;
  ~String() noexcept;

  String(const std::string &);
  String(const char *);
  String(const char *, std::size_t);
  String(const char16_t *);
  String(const char16_t *, std::size_t);
#ifdef __cpp_char8_t
  String(const char8_t *s);
  String(const char8_t *s, std::size_t len);
#endif

  static String lossy(const std::string &) noexcept;
  static String lossy(const char *) noexcept;
  static String lossy(const char *, std::size_t) noexcept;
  static String lossy(const char16_t *) noexcept;
  static String lossy(const char16_t *, std::size_t) noexcept;

  String &operator=(const String &) & noexcept;
  String &operator=(String &&) & noexcept;

  explicit operator std::string() const;

  const char *data() const noexcept;
  std::size_t size() const noexcept;
  std::size_t length() const noexcept;
  bool empty() const noexcept;

  const char *c_str() noexcept;

  std::size_t capacity() const noexcept;
  void reserve(size_t new_cap) noexcept;

  using iterator = char *;
  iterator begin() noexcept;
  iterator end() noexcept;

  using const_iterator = const char *;
  const_iterator begin() const noexcept;
  const_iterator end() const noexcept;
  const_iterator cbegin() const noexcept;
  const_iterator cend() const noexcept;

  bool operator==(const String &) const noexcept;
  bool operator!=(const String &) const noexcept;
  bool operator<(const String &) const noexcept;
  bool operator<=(const String &) const noexcept;
  bool operator>(const String &) const noexcept;
  bool operator>=(const String &) const noexcept;

  void swap(String &) noexcept;

  String(unsafe_bitcopy_t, const String &) noexcept;

private:
  struct lossy_t;
  String(lossy_t, const char *, std::size_t) noexcept;
  String(lossy_t, const char16_t *, std::size_t) noexcept;
  friend void swap(String &lhs, String &rhs) noexcept { lhs.swap(rhs); }

  std::array<std::uintptr_t, 3> repr;
};
#endif // CXXBRIDGE1_RUST_STRING

#ifndef CXXBRIDGE1_RUST_STR
#define CXXBRIDGE1_RUST_STR
class Str final {
public:
  Str() noexcept;
  Str(const String &) noexcept;
  Str(const std::string &);
  Str(const char *);
  Str(const char *, std::size_t);

  Str &operator=(const Str &) & noexcept = default;

  explicit operator std::string() const;
#if __cplusplus >= 201703L
  explicit operator std::string_view() const;
#endif

  const char *data() const noexcept;
  std::size_t size() const noexcept;
  std::size_t length() const noexcept;
  bool empty() const noexcept;

  Str(const Str &) noexcept = default;
  ~Str() noexcept = default;

  using iterator = const char *;
  using const_iterator = const char *;
  const_iterator begin() const noexcept;
  const_iterator end() const noexcept;
  const_iterator cbegin() const noexcept;
  const_iterator cend() const noexcept;

  bool operator==(const Str &) const noexcept;
  bool operator!=(const Str &) const noexcept;
  bool operator<(const Str &) const noexcept;
  bool operator<=(const Str &) const noexcept;
  bool operator>(const Str &) const noexcept;
  bool operator>=(const Str &) const noexcept;

  void swap(Str &) noexcept;

private:
  class uninit;
  Str(uninit) noexcept;
  friend impl<Str>;

  std::array<std::uintptr_t, 2> repr;
};
#endif // CXXBRIDGE1_RUST_STR

#ifndef CXXBRIDGE1_RUST_SLICE
#define CXXBRIDGE1_RUST_SLICE
namespace detail {
template <bool>
struct copy_assignable_if {};

template <>
struct copy_assignable_if<false> {
  copy_assignable_if() noexcept = default;
  copy_assignable_if(const copy_assignable_if &) noexcept = default;
  copy_assignable_if &operator=(const copy_assignable_if &) & noexcept = delete;
  copy_assignable_if &operator=(copy_assignable_if &&) & noexcept = default;
};
} // namespace detail

template <typename T>
class Slice final
    : private detail::copy_assignable_if<std::is_const<T>::value> {
public:
  using value_type = T;

  Slice() noexcept;
  Slice(T *, std::size_t count) noexcept;

  template <typename C>
  explicit Slice(C &c) : Slice(c.data(), c.size()) {}

  Slice &operator=(const Slice<T> &) & noexcept = default;
  Slice &operator=(Slice<T> &&) & noexcept = default;

  T *data() const noexcept;
  std::size_t size() const noexcept;
  std::size_t length() const noexcept;
  bool empty() const noexcept;

  T &operator[](std::size_t n) const noexcept;
  T &at(std::size_t n) const;
  T &front() const noexcept;
  T &back() const noexcept;

  Slice(const Slice<T> &) noexcept = default;
  ~Slice() noexcept = default;

  class iterator;
  iterator begin() const noexcept;
  iterator end() const noexcept;

  void swap(Slice &) noexcept;

private:
  class uninit;
  Slice(uninit) noexcept;
  friend impl<Slice>;
  friend void sliceInit(void *, const void *, std::size_t) noexcept;
  friend void *slicePtr(const void *) noexcept;
  friend std::size_t sliceLen(const void *) noexcept;

  std::array<std::uintptr_t, 2> repr;
};

#ifdef __cpp_deduction_guides
template <typename C>
explicit Slice(C &c)
    -> Slice<std::remove_reference_t<decltype(*std::declval<C>().data())>>;
#endif // __cpp_deduction_guides

template <typename T>
class Slice<T>::iterator final {
public:
#if __cplusplus >= 202002L
  using iterator_category = std::contiguous_iterator_tag;
#else
  using iterator_category = std::random_access_iterator_tag;
#endif
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = typename std::add_pointer<T>::type;
  using reference = typename std::add_lvalue_reference<T>::type;

  reference operator*() const noexcept;
  pointer operator->() const noexcept;
  reference operator[](difference_type) const noexcept;

  iterator &operator++() noexcept;
  iterator operator++(int) noexcept;
  iterator &operator--() noexcept;
  iterator operator--(int) noexcept;

  iterator &operator+=(difference_type) noexcept;
  iterator &operator-=(difference_type) noexcept;
  iterator operator+(difference_type) const noexcept;
  friend inline iterator operator+(difference_type lhs, iterator rhs) noexcept {
    return rhs + lhs;
  }
  iterator operator-(difference_type) const noexcept;
  difference_type operator-(const iterator &) const noexcept;

  bool operator==(const iterator &) const noexcept;
  bool operator!=(const iterator &) const noexcept;
  bool operator<(const iterator &) const noexcept;
  bool operator<=(const iterator &) const noexcept;
  bool operator>(const iterator &) const noexcept;
  bool operator>=(const iterator &) const noexcept;

private:
  friend class Slice;
  void *pos;
  std::size_t stride;
};

#if __cplusplus >= 202002L
static_assert(std::ranges::contiguous_range<rust::Slice<const uint8_t>>);
static_assert(std::contiguous_iterator<rust::Slice<const uint8_t>::iterator>);
#endif

template <typename T>
Slice<T>::Slice() noexcept {
  sliceInit(this, reinterpret_cast<void *>(align_of<T>()), 0);
}

template <typename T>
Slice<T>::Slice(T *s, std::size_t count) noexcept {
  assert(s != nullptr || count == 0);
  sliceInit(this,
            s == nullptr && count == 0
                ? reinterpret_cast<void *>(align_of<T>())
                : const_cast<typename std::remove_const<T>::type *>(s),
            count);
}

template <typename T>
T *Slice<T>::data() const noexcept {
  return reinterpret_cast<T *>(slicePtr(this));
}

template <typename T>
std::size_t Slice<T>::size() const noexcept {
  return sliceLen(this);
}

template <typename T>
std::size_t Slice<T>::length() const noexcept {
  return this->size();
}

template <typename T>
bool Slice<T>::empty() const noexcept {
  return this->size() == 0;
}

template <typename T>
T &Slice<T>::operator[](std::size_t n) const noexcept {
  assert(n < this->size());
  auto ptr = static_cast<char *>(slicePtr(this)) + size_of<T>() * n;
  return *reinterpret_cast<T *>(ptr);
}

template <typename T>
T &Slice<T>::at(std::size_t n) const {
  if (n >= this->size()) {
    panic<std::out_of_range>("rust::Slice index out of range");
  }
  return (*this)[n];
}

template <typename T>
T &Slice<T>::front() const noexcept {
  assert(!this->empty());
  return (*this)[0];
}

template <typename T>
T &Slice<T>::back() const noexcept {
  assert(!this->empty());
  return (*this)[this->size() - 1];
}

template <typename T>
typename Slice<T>::iterator::reference
Slice<T>::iterator::operator*() const noexcept {
  return *static_cast<T *>(this->pos);
}

template <typename T>
typename Slice<T>::iterator::pointer
Slice<T>::iterator::operator->() const noexcept {
  return static_cast<T *>(this->pos);
}

template <typename T>
typename Slice<T>::iterator::reference Slice<T>::iterator::operator[](
    typename Slice<T>::iterator::difference_type n) const noexcept {
  auto ptr = static_cast<char *>(this->pos) + this->stride * n;
  return *reinterpret_cast<T *>(ptr);
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator++() noexcept {
  this->pos = static_cast<char *>(this->pos) + this->stride;
  return *this;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator++(int) noexcept {
  auto ret = iterator(*this);
  this->pos = static_cast<char *>(this->pos) + this->stride;
  return ret;
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator--() noexcept {
  this->pos = static_cast<char *>(this->pos) - this->stride;
  return *this;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator--(int) noexcept {
  auto ret = iterator(*this);
  this->pos = static_cast<char *>(this->pos) - this->stride;
  return ret;
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator+=(
    typename Slice<T>::iterator::difference_type n) noexcept {
  this->pos = static_cast<char *>(this->pos) + this->stride * n;
  return *this;
}

template <typename T>
typename Slice<T>::iterator &Slice<T>::iterator::operator-=(
    typename Slice<T>::iterator::difference_type n) noexcept {
  this->pos = static_cast<char *>(this->pos) - this->stride * n;
  return *this;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator+(
    typename Slice<T>::iterator::difference_type n) const noexcept {
  auto ret = iterator(*this);
  ret.pos = static_cast<char *>(this->pos) + this->stride * n;
  return ret;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::iterator::operator-(
    typename Slice<T>::iterator::difference_type n) const noexcept {
  auto ret = iterator(*this);
  ret.pos = static_cast<char *>(this->pos) - this->stride * n;
  return ret;
}

template <typename T>
typename Slice<T>::iterator::difference_type
Slice<T>::iterator::operator-(const iterator &other) const noexcept {
  auto diff = std::distance(static_cast<char *>(other.pos),
                            static_cast<char *>(this->pos));
  return diff / static_cast<typename Slice<T>::iterator::difference_type>(
                    this->stride);
}

template <typename T>
bool Slice<T>::iterator::operator==(const iterator &other) const noexcept {
  return this->pos == other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator!=(const iterator &other) const noexcept {
  return this->pos != other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator<(const iterator &other) const noexcept {
  return this->pos < other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator<=(const iterator &other) const noexcept {
  return this->pos <= other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator>(const iterator &other) const noexcept {
  return this->pos > other.pos;
}

template <typename T>
bool Slice<T>::iterator::operator>=(const iterator &other) const noexcept {
  return this->pos >= other.pos;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::begin() const noexcept {
  iterator it;
  it.pos = slicePtr(this);
  it.stride = size_of<T>();
  return it;
}

template <typename T>
typename Slice<T>::iterator Slice<T>::end() const noexcept {
  iterator it = this->begin();
  it.pos = static_cast<char *>(it.pos) + it.stride * this->size();
  return it;
}

template <typename T>
void Slice<T>::swap(Slice &rhs) noexcept {
  std::swap(*this, rhs);
}
#endif // CXXBRIDGE1_RUST_SLICE

#ifndef CXXBRIDGE1_RUST_BOX
#define CXXBRIDGE1_RUST_BOX
template <typename T>
class Box final {
public:
  using element_type = T;
  using const_pointer =
      typename std::add_pointer<typename std::add_const<T>::type>::type;
  using pointer = typename std::add_pointer<T>::type;

  Box() = delete;
  Box(Box &&) noexcept;
  ~Box() noexcept;

  explicit Box(const T &);
  explicit Box(T &&);

  Box &operator=(Box &&) & noexcept;

  const T *operator->() const noexcept;
  const T &operator*() const noexcept;
  T *operator->() noexcept;
  T &operator*() noexcept;

  template <typename... Fields>
  static Box in_place(Fields &&...);

  void swap(Box &) noexcept;

  static Box from_raw(T *) noexcept;

  T *into_raw() noexcept;

  /* Deprecated */ using value_type = element_type;

private:
  class uninit;
  class allocation;
  Box(uninit) noexcept;
  void drop() noexcept;

  friend void swap(Box &lhs, Box &rhs) noexcept { lhs.swap(rhs); }

  T *ptr;
};

template <typename T>
class Box<T>::uninit {};

template <typename T>
class Box<T>::allocation {
  static T *alloc() noexcept;
  static void dealloc(T *) noexcept;

public:
  allocation() noexcept : ptr(alloc()) {}
  ~allocation() noexcept {
    if (this->ptr) {
      dealloc(this->ptr);
    }
  }
  T *ptr;
};

template <typename T>
Box<T>::Box(Box &&other) noexcept : ptr(other.ptr) {
  other.ptr = nullptr;
}

template <typename T>
Box<T>::Box(const T &val) {
  allocation alloc;
  ::new (alloc.ptr) T(val);
  this->ptr = alloc.ptr;
  alloc.ptr = nullptr;
}

template <typename T>
Box<T>::Box(T &&val) {
  allocation alloc;
  ::new (alloc.ptr) T(std::move(val));
  this->ptr = alloc.ptr;
  alloc.ptr = nullptr;
}

template <typename T>
Box<T>::~Box() noexcept {
  if (this->ptr) {
    this->drop();
  }
}

template <typename T>
Box<T> &Box<T>::operator=(Box &&other) & noexcept {
  if (this->ptr) {
    this->drop();
  }
  this->ptr = other.ptr;
  other.ptr = nullptr;
  return *this;
}

template <typename T>
const T *Box<T>::operator->() const noexcept {
  return this->ptr;
}

template <typename T>
const T &Box<T>::operator*() const noexcept {
  return *this->ptr;
}

template <typename T>
T *Box<T>::operator->() noexcept {
  return this->ptr;
}

template <typename T>
T &Box<T>::operator*() noexcept {
  return *this->ptr;
}

template <typename T>
template <typename... Fields>
Box<T> Box<T>::in_place(Fields &&...fields) {
  allocation alloc;
  auto ptr = alloc.ptr;
  ::new (ptr) T{std::forward<Fields>(fields)...};
  alloc.ptr = nullptr;
  return from_raw(ptr);
}

template <typename T>
void Box<T>::swap(Box &rhs) noexcept {
  using std::swap;
  swap(this->ptr, rhs.ptr);
}

template <typename T>
Box<T> Box<T>::from_raw(T *raw) noexcept {
  Box box = uninit{};
  box.ptr = raw;
  return box;
}

template <typename T>
T *Box<T>::into_raw() noexcept {
  T *raw = this->ptr;
  this->ptr = nullptr;
  return raw;
}

template <typename T>
Box<T>::Box(uninit) noexcept {}
#endif // CXXBRIDGE1_RUST_BOX

#ifndef CXXBRIDGE1_RUST_BITCOPY_T
#define CXXBRIDGE1_RUST_BITCOPY_T
struct unsafe_bitcopy_t final {
  explicit unsafe_bitcopy_t() = default;
};
#endif // CXXBRIDGE1_RUST_BITCOPY_T

#ifndef CXXBRIDGE1_RUST_VEC
#define CXXBRIDGE1_RUST_VEC
template <typename T>
class Vec final {
public:
  using value_type = T;

  Vec() noexcept;
  Vec(std::initializer_list<T>);
  Vec(const Vec &);
  Vec(Vec &&) noexcept;
  ~Vec() noexcept;

  Vec &operator=(Vec &&) & noexcept;
  Vec &operator=(const Vec &) &;

  std::size_t size() const noexcept;
  bool empty() const noexcept;
  const T *data() const noexcept;
  T *data() noexcept;
  std::size_t capacity() const noexcept;

  const T &operator[](std::size_t n) const noexcept;
  const T &at(std::size_t n) const;
  const T &front() const noexcept;
  const T &back() const noexcept;

  T &operator[](std::size_t n) noexcept;
  T &at(std::size_t n);
  T &front() noexcept;
  T &back() noexcept;

  void reserve(std::size_t new_cap);
  void push_back(const T &value);
  void push_back(T &&value);
  template <typename... Args>
  void emplace_back(Args &&...args);
  void truncate(std::size_t len);
  void clear();

  using iterator = typename Slice<T>::iterator;
  iterator begin() noexcept;
  iterator end() noexcept;

  using const_iterator = typename Slice<const T>::iterator;
  const_iterator begin() const noexcept;
  const_iterator end() const noexcept;
  const_iterator cbegin() const noexcept;
  const_iterator cend() const noexcept;

  void swap(Vec &) noexcept;

  Vec(unsafe_bitcopy_t, const Vec &) noexcept;

private:
  void reserve_total(std::size_t new_cap) noexcept;
  void set_len(std::size_t len) noexcept;
  void drop() noexcept;

  friend void swap(Vec &lhs, Vec &rhs) noexcept { lhs.swap(rhs); }

  std::array<std::uintptr_t, 3> repr;
};

template <typename T>
Vec<T>::Vec(std::initializer_list<T> init) : Vec{} {
  this->reserve_total(init.size());
  std::move(init.begin(), init.end(), std::back_inserter(*this));
}

template <typename T>
Vec<T>::Vec(const Vec &other) : Vec() {
  this->reserve_total(other.size());
  std::copy(other.begin(), other.end(), std::back_inserter(*this));
}

template <typename T>
Vec<T>::Vec(Vec &&other) noexcept : repr(other.repr) {
  new (&other) Vec();
}

template <typename T>
Vec<T>::~Vec() noexcept {
  this->drop();
}

template <typename T>
Vec<T> &Vec<T>::operator=(Vec &&other) & noexcept {
  this->drop();
  this->repr = other.repr;
  new (&other) Vec();
  return *this;
}

template <typename T>
Vec<T> &Vec<T>::operator=(const Vec &other) & {
  if (this != &other) {
    this->drop();
    new (this) Vec(other);
  }
  return *this;
}

template <typename T>
bool Vec<T>::empty() const noexcept {
  return this->size() == 0;
}

template <typename T>
T *Vec<T>::data() noexcept {
  return const_cast<T *>(const_cast<const Vec<T> *>(this)->data());
}

template <typename T>
const T &Vec<T>::operator[](std::size_t n) const noexcept {
  assert(n < this->size());
  auto data = reinterpret_cast<const char *>(this->data());
  return *reinterpret_cast<const T *>(data + n * size_of<T>());
}

template <typename T>
const T &Vec<T>::at(std::size_t n) const {
  if (n >= this->size()) {
    panic<std::out_of_range>("rust::Vec index out of range");
  }
  return (*this)[n];
}

template <typename T>
const T &Vec<T>::front() const noexcept {
  assert(!this->empty());
  return (*this)[0];
}

template <typename T>
const T &Vec<T>::back() const noexcept {
  assert(!this->empty());
  return (*this)[this->size() - 1];
}

template <typename T>
T &Vec<T>::operator[](std::size_t n) noexcept {
  assert(n < this->size());
  auto data = reinterpret_cast<char *>(this->data());
  return *reinterpret_cast<T *>(data + n * size_of<T>());
}

template <typename T>
T &Vec<T>::at(std::size_t n) {
  if (n >= this->size()) {
    panic<std::out_of_range>("rust::Vec index out of range");
  }
  return (*this)[n];
}

template <typename T>
T &Vec<T>::front() noexcept {
  assert(!this->empty());
  return (*this)[0];
}

template <typename T>
T &Vec<T>::back() noexcept {
  assert(!this->empty());
  return (*this)[this->size() - 1];
}

template <typename T>
void Vec<T>::reserve(std::size_t new_cap) {
  this->reserve_total(new_cap);
}

template <typename T>
void Vec<T>::push_back(const T &value) {
  this->emplace_back(value);
}

template <typename T>
void Vec<T>::push_back(T &&value) {
  this->emplace_back(std::move(value));
}

template <typename T>
template <typename... Args>
void Vec<T>::emplace_back(Args &&...args) {
  auto size = this->size();
  this->reserve_total(size + 1);
  ::new (reinterpret_cast<T *>(reinterpret_cast<char *>(this->data()) +
                               size * size_of<T>()))
      T(std::forward<Args>(args)...);
  this->set_len(size + 1);
}

template <typename T>
void Vec<T>::clear() {
  this->truncate(0);
}

template <typename T>
typename Vec<T>::iterator Vec<T>::begin() noexcept {
  return Slice<T>(this->data(), this->size()).begin();
}

template <typename T>
typename Vec<T>::iterator Vec<T>::end() noexcept {
  return Slice<T>(this->data(), this->size()).end();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::begin() const noexcept {
  return this->cbegin();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::end() const noexcept {
  return this->cend();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::cbegin() const noexcept {
  return Slice<const T>(this->data(), this->size()).begin();
}

template <typename T>
typename Vec<T>::const_iterator Vec<T>::cend() const noexcept {
  return Slice<const T>(this->data(), this->size()).end();
}

template <typename T>
void Vec<T>::swap(Vec &rhs) noexcept {
  using std::swap;
  swap(this->repr, rhs.repr);
}

template <typename T>
Vec<T>::Vec(unsafe_bitcopy_t, const Vec &bits) noexcept : repr(bits.repr) {}
#endif // CXXBRIDGE1_RUST_VEC

#ifndef CXXBRIDGE1_RUST_OPAQUE
#define CXXBRIDGE1_RUST_OPAQUE
class Opaque {
public:
  Opaque() = delete;
  Opaque(const Opaque &) = delete;
  ~Opaque() = delete;
};
#endif // CXXBRIDGE1_RUST_OPAQUE

#ifndef CXXBRIDGE1_IS_COMPLETE
#define CXXBRIDGE1_IS_COMPLETE
namespace detail {
namespace {
template <typename T, typename = std::size_t>
struct is_complete : std::false_type {};
template <typename T>
struct is_complete<T, decltype(sizeof(T))> : std::true_type {};
} // namespace
} // namespace detail
#endif // CXXBRIDGE1_IS_COMPLETE

#ifndef CXXBRIDGE1_LAYOUT
#define CXXBRIDGE1_LAYOUT
class layout {
  template <typename T>
  friend std::size_t size_of();
  template <typename T>
  friend std::size_t align_of();
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_size_of() {
    return T::layout::size();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_size_of() {
    return sizeof(T);
  }
  template <typename T>
  static
      typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
      size_of() {
    return do_size_of<T>();
  }
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_align_of() {
    return T::layout::align();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_align_of() {
    return alignof(T);
  }
  template <typename T>
  static
      typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
      align_of() {
    return do_align_of<T>();
  }
};

template <typename T>
std::size_t size_of() {
  return layout::size_of<T>();
}

template <typename T>
std::size_t align_of() {
  return layout::align_of<T>();
}
#endif // CXXBRIDGE1_LAYOUT
} // namespace cxxbridge1
} // namespace rust

#if __cplusplus >= 201402L
#define CXX_DEFAULT_VALUE(value) = value
#else
#define CXX_DEFAULT_VALUE(value)
#endif

enum class Network : ::std::uint8_t;
enum class LogLevel : ::std::uint8_t;
enum class NotificationFlag : ::std::uint8_t;
enum class HwiDeviceState : ::std::uint8_t;
enum class HwiEventFlag : ::std::uint8_t;
struct Notification;
struct RustCoin;
struct CoinState;
struct RustTx;
struct Output;
struct TransactionTemplate;
struct TransactionSimulation;
struct TxResult;
struct ConnectionResult;
struct RegtestDefaults;
struct PsbtValidation;
struct BackendInfo;
struct HwiDevice;
struct HwiEvent;
struct Config;
struct Account;
struct Poll;
struct PsbtResult;
struct NotificationReceiver;
struct SyncEstimator;
struct HwiService;
struct HwiReceiver;
struct HwiPoll;

#ifndef CXXBRIDGE1_ENUM_Network
#define CXXBRIDGE1_ENUM_Network
// Bitcoin network types.
enum class Network : ::std::uint8_t {
  Regtest = 0,
  Signet = 1,
  Testnet = 2,
  Bitcoin = 3,
};
#endif // CXXBRIDGE1_ENUM_Network

#ifndef CXXBRIDGE1_ENUM_LogLevel
#define CXXBRIDGE1_ENUM_LogLevel
// Logging levels.
enum class LogLevel : ::std::uint8_t {
  Off = 0,
  Error = 1,
  Warn = 2,
  Info = 3,
  Debug = 4,
  Trace = 5,
};
#endif // CXXBRIDGE1_ENUM_LogLevel

#ifndef CXXBRIDGE1_ENUM_NotificationFlag
#define CXXBRIDGE1_ENUM_NotificationFlag
// Notification types from the scanner.
enum class NotificationFlag : ::std::uint8_t {
  StartingScan = 0,
  ScanStarted = 1,
  FailStartScanning = 2,
  FailScan = 3,
  StoppingScan = 4,
  ScanStopped = 5,
  ScanProgress = 6,
  ScanCompleted = 7,
  NewOutput = 8,
  OutputSpent = 9,
  WaitingForBlocks = 10,
  NewBlocksDetected = 11,
  CoinUpdate = 12,
  PaymentUpdated = 13,
  HeaderProgress = 14,
  AddressTipChanged = 15,
  ElectrumStarted = 16,
  ElectrumConnected = 17,
  ElectrumError = 18,
  ElectrumStopped = 19,
};
#endif // CXXBRIDGE1_ENUM_NotificationFlag

#ifndef CXXBRIDGE1_ENUM_HwiDeviceState
#define CXXBRIDGE1_ENUM_HwiDeviceState
// State of a signing device the HWI service can see.
enum class HwiDeviceState : ::std::uint8_t {
  Supported = 0,
  Locked = 1,
  Unsupported = 2,
};
#endif // CXXBRIDGE1_ENUM_HwiDeviceState

#ifndef CXXBRIDGE1_ENUM_HwiEventFlag
#define CXXBRIDGE1_ENUM_HwiEventFlag
// Kind of notification coming from the HWI service.
enum class HwiEventFlag : ::std::uint8_t {
  DevicesChanged = 0,
  Xpub = 1,
  Version = 2,
  WalletRegistered = 3,
  WalletIsRegistered = 4,
  TransactionSigned = 5,
  Error = 6,
};
#endif // CXXBRIDGE1_ENUM_HwiEventFlag

#ifndef CXXBRIDGE1_STRUCT_Notification
#define CXXBRIDGE1_STRUCT_Notification
// Notification from the account scanner.
struct Notification final {
  ::NotificationFlag flag;
  ::rust::String payload;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_Notification

#ifndef CXXBRIDGE1_STRUCT_RustCoin
#define CXXBRIDGE1_STRUCT_RustCoin
// A coin entry from the wallet.
struct RustCoin final {
  ::rust::String outpoint;
  ::std::uint64_t value CXX_DEFAULT_VALUE(0);
  ::std::uint32_t height CXX_DEFAULT_VALUE(0);
  ::rust::String label;
  bool spent CXX_DEFAULT_VALUE(false);
  ::rust::String account_type;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustCoin

#ifndef CXXBRIDGE1_STRUCT_CoinState
#define CXXBRIDGE1_STRUCT_CoinState
// Summary of spendable coins.
struct CoinState final {
  ::std::uint64_t confirmed_count CXX_DEFAULT_VALUE(0);
  ::std::uint64_t confirmed_balance CXX_DEFAULT_VALUE(0);
  ::std::uint64_t unconfirmed_count CXX_DEFAULT_VALUE(0);
  ::std::uint64_t unconfirmed_balance CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_CoinState

#ifndef CXXBRIDGE1_STRUCT_RustTx
#define CXXBRIDGE1_STRUCT_RustTx
// A transaction entry from payment history.
struct RustTx final {
  ::rust::String txid;
  ::rust::String direction;
  ::std::uint64_t amount CXX_DEFAULT_VALUE(0);
  ::std::uint64_t fee CXX_DEFAULT_VALUE(0);
  ::std::uint32_t height CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustTx

#ifndef CXXBRIDGE1_STRUCT_Output
#define CXXBRIDGE1_STRUCT_Output
// Transaction output specification.
struct Output final {
  // Recipient address (SP address, legacy address, or hex data for OP_RETURN)
  ::rust::String address;
  // Amount in satoshis (must be 0 for OP_RETURN outputs)
  ::std::uint64_t amount CXX_DEFAULT_VALUE(0);
  // Optional label for this output
  ::rust::String label;
  // If true, send maximum possible amount (minus fees) to this output
  bool max CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_Output

#ifndef CXXBRIDGE1_STRUCT_TransactionTemplate
#define CXXBRIDGE1_STRUCT_TransactionTemplate
// Template for creating a transaction.
struct TransactionTemplate final {
  // List of output specifications
  ::rust::Vec<::Output> outputs;
  // Fee rate in sat/vbyte (used when fee == 0)
  double fee_rate CXX_DEFAULT_VALUE(0);
  // Absolute fee in satoshis (takes precedence over fee_rate when > 0)
  ::std::uint64_t fee CXX_DEFAULT_VALUE(0);
  // Optional: specific UTXOs to use (empty = automatic selection)
  ::rust::Vec<::rust::String> input_outpoints;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_TransactionTemplate

#ifndef CXXBRIDGE1_STRUCT_TransactionSimulation
#define CXXBRIDGE1_STRUCT_TransactionSimulation
// Result of transaction simulation.
struct TransactionSimulation final {
  // Whether the transaction can be created with current funds
  bool is_valid CXX_DEFAULT_VALUE(false);
  // Estimated fee in satoshis (0 if invalid)
  ::std::uint64_t fee CXX_DEFAULT_VALUE(0);
  // Estimated weight in weight units (0 if invalid)
  ::std::uint64_t weight CXX_DEFAULT_VALUE(0);
  // Total input amount in satoshis (0 if invalid)
  ::std::uint64_t input_total CXX_DEFAULT_VALUE(0);
  // Total output amount in satoshis (0 if invalid)
  ::std::uint64_t output_total CXX_DEFAULT_VALUE(0);
  // Number of inputs selected (0 if invalid)
  ::std::uint64_t input_count CXX_DEFAULT_VALUE(0);
  // Error message (empty if valid)
  ::rust::String error;
  // Outpoints selected by coin selection (empty if invalid)
  ::rust::Vec<::rust::String> selected_outpoints;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_TransactionSimulation

#ifndef CXXBRIDGE1_STRUCT_TxResult
#define CXXBRIDGE1_STRUCT_TxResult
// Result of a transaction operation (sign, broadcast).
struct TxResult final {
  // Whether the operation succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // Result value: signed tx hex or txid (empty on error).
  ::rust::String value;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_TxResult

#ifndef CXXBRIDGE1_STRUCT_ConnectionResult
#define CXXBRIDGE1_STRUCT_ConnectionResult
// Result of a connection test.
struct ConnectionResult final {
  // Whether the connection succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // Normalized URL that successfully connected (with scheme).
  ::rust::String url;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_ConnectionResult

#ifndef CXXBRIDGE1_STRUCT_RegtestDefaults
#define CXXBRIDGE1_STRUCT_RegtestDefaults
// Default regtest infrastructure addresses fetched from minta API.
struct RegtestDefaults final {
  // Whether the fetch succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // BlindBit server URL (with http:// prefix).
  ::rust::String blindbit_url;
  // Electrum server address (host:port).
  ::rust::String electrum_url;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RegtestDefaults

#ifndef CXXBRIDGE1_STRUCT_PsbtValidation
#define CXXBRIDGE1_STRUCT_PsbtValidation
// Result of PSBT pre-signing validation via Electrum.
struct PsbtValidation final {
  // Whether the validation check itself succeeded (false = connection/protocol error).
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message when is_ok is false (connection failure, etc.).
  ::rust::String error;
  // Whether the PSBT passed all checks (true = no issues found).
  bool is_valid CXX_DEFAULT_VALUE(false);
  // Human-readable summary of issues found.
  ::rust::String issues;
  // Number of output addresses that have been used before.
  ::std::uint32_t reused_output_count CXX_DEFAULT_VALUE(0);
  // Number of inputs whose coins have already been spent.
  ::std::uint32_t spent_input_count CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PsbtValidation

#ifndef CXXBRIDGE1_STRUCT_BackendInfo
#define CXXBRIDGE1_STRUCT_BackendInfo
// Backend server information result.
struct BackendInfo final {
  // Whether the request succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // Normalized URL that successfully connected (with scheme).
  ::rust::String url;
  // Network reported by the backend.
  ::Network network;
  // Current block height.
  ::std::uint32_t height CXX_DEFAULT_VALUE(0);
  // Backend supports tweaks-only mode.
  bool tweaks_only CXX_DEFAULT_VALUE(false);
  // Backend supports full basic tweaks.
  bool tweaks_full_basic CXX_DEFAULT_VALUE(false);
  // Backend supports full tweaks with dust filter.
  bool tweaks_full_with_dust_filter CXX_DEFAULT_VALUE(false);
  // Backend supports cut-through with dust filter.
  bool tweaks_cut_through_with_dust_filter CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_BackendInfo

#ifndef CXXBRIDGE1_STRUCT_HwiDevice
#define CXXBRIDGE1_STRUCT_HwiDevice
// One signing device as the service currently sees it.
struct HwiDevice final {
  // Stable, serial-based where the device provides one.
  ::rust::String id;
  ::rust::String kind;
  // Empty unless the device is Supported.
  ::rust::String fingerprint;
  // Empty when the device did not report one.
  ::rust::String version;
  ::HwiDeviceState state;
  // Pairing code when Locked, the reason when Unsupported, else empty.
  ::rust::String detail;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_HwiDevice

#ifndef CXXBRIDGE1_STRUCT_HwiEvent
#define CXXBRIDGE1_STRUCT_HwiEvent
// A notification from the HWI service. `req_id` is the id the caller passed to the
// request it answers, and 0 for an unsolicited push.
struct HwiEvent final {
  ::HwiEventFlag flag;
  ::std::uint64_t req_id CXX_DEFAULT_VALUE(0);
  ::rust::String fingerprint;
  ::rust::String payload;
  ::rust::String error;
  // WalletIsRegistered answers with it; other kinds set it on success.
  bool ok CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_HwiEvent

#ifndef CXXBRIDGE1_STRUCT_Config
#define CXXBRIDGE1_STRUCT_Config
// Configuration for a Silent account.
struct Config final : public ::rust::Opaque {
  // Check if the config was loaded successfully.
  bool is_ok() const noexcept;

  // Get error message (empty if is_ok).
  ::rust::String get_error() const noexcept;

  // Save config to file.
  void to_file() const noexcept;

  // Get mnemonic.
  ::rust::String get_mnemonic() const noexcept;

  // Set mnemonic.
  void set_mnemonic(::rust::String mnemonic) noexcept;

  // Get network.
  ::Network get_network() const noexcept;

  // Set network.
  void set_network(::Network network) noexcept;

  // Get BlindBit URL.
  ::rust::String get_blindbit_url() const noexcept;

  // Set BlindBit URL.
  void set_blindbit_url(::rust::String url) noexcept;

  // Get Electrum URL.
  ::rust::String get_electrum_url() const noexcept;

  // Set Electrum URL.
  void set_electrum_url(::rust::String url) noexcept;

  // Get dust limit (0 means not set).
  ::std::uint64_t get_dust_limit() const noexcept;

  // Set dust limit (0 to unset).
  void set_dust_limit(::std::uint64_t limit) noexcept;

  // Get plugin/module identifier.
  ::rust::String get_plugin_id() const noexcept;

  // Set plugin/module identifier.
  void set_plugin_id(::rust::String plugin_id) noexcept;

  ~Config() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_Config

#ifndef CXXBRIDGE1_STRUCT_Account
#define CXXBRIDGE1_STRUCT_Account
// Account instance.
struct Account final : public ::rust::Opaque {
  // Check if the account was created successfully.
  bool is_ok() const noexcept;

  // Get error message (empty if is_ok).
  ::rust::String get_error() const noexcept;

  // Start the scanner.
  bool start_scanner() noexcept;

  // Stop the scanner.
  void stop_scanner() noexcept;

  // Stop all electrum listeners on sub-accounts.
  void stop_electrum() noexcept;

  // Reload electrum config and start listeners.
  bool start_electrum() noexcept;

  // Try to receive a notification (non-blocking).
  ::rust::Box<::Poll> try_recv() noexcept;

  // Take ownership of the notification receiver for use in a dedicated thread.
  ::rust::Box<::NotificationReceiver> take_receiver() noexcept;

  // Get account name.
  ::rust::String name() const noexcept;

  // Get balance in satoshis.
  ::std::uint64_t balance() const noexcept;

  // Get the Silent Payment address.
  ::rust::String sp_address() const noexcept;

  // Check if the account has sub-accounts (segwit + taproot).
  bool has_sub_accounts() const noexcept;

  // Get the number of sub-accounts.
  ::std::uint32_t sub_account_count() const noexcept;

  // Generate a new segwit (wpkh) receiving address.
  ::rust::String new_segwit_addr() noexcept;

  // Generate a new taproot receiving address.
  ::rust::String new_taproot_addr() noexcept;

  // Get all coins (spent and unspent).
  ::rust::Vec<::RustCoin> coins() const noexcept;

  // Get spendable coins summary.
  ::CoinState spendable_coins() const noexcept;

  // Update a coin's label.
  bool update_coin_label(::rust::String outpoint, ::rust::String label) noexcept;

  // Get payment history.
  ::rust::Vec<::RustTx> payment_history() const noexcept;

  // Simulate a transaction to check feasibility and estimate fees.
  // Returns simulation result with fee, weight, and validity info.
  ::TransactionSimulation simulate_transaction(::TransactionTemplate tx_template) const noexcept;

  // Prepare a transaction for signing.
  // Creates an unsigned transaction from the template and returns a PsbtResult.
  // The transaction will be finalized and ready to sign.
  ::rust::Box<::PsbtResult> prepare_transaction(::TransactionTemplate tx_template) const noexcept;

  // Sign a prepared transaction.
  // Returns TxResult with signed tx hex in value.
  ::TxResult sign_transaction(::PsbtResult const &psbt_result) const noexcept;

  // Broadcast a signed transaction (hex string) to the network.
  // Returns TxResult with txid in value.
  ::TxResult broadcast_transaction(::rust::String signed_tx_hex) const noexcept;

  // Sign and broadcast a transaction in one step.
  // Returns TxResult with txid in value.
  ::TxResult sign_and_broadcast(::PsbtResult const &psbt_result) const noexcept;

  // Log all details of a failed broadcast for debugging/reproduction.
  // Called after broadcast_transaction returns an error.
  void log_failed_broadcast(::TransactionTemplate tx_template, ::rust::String signed_tx_hex) const noexcept;

  // Validate a prepared transaction via Electrum before signing.
  // Checks for output address reuse and already-spent inputs.
  // Blocking call - must be called from a background thread.
  ::PsbtValidation validate_before_sign(::PsbtResult const &psbt_result) const noexcept;

  ~Account() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_Account

#ifndef CXXBRIDGE1_STRUCT_Poll
#define CXXBRIDGE1_STRUCT_Poll
// Poll result for checking notifications.
struct Poll final : public ::rust::Opaque {
  // Check if poll has a notification.
  bool is_some() const noexcept;

  // Get the notification (only valid if is_some() == true).
  ::Notification get_notification() const noexcept;

  // Get error message (if any).
  ::rust::String get_error() const noexcept;

  ~Poll() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_Poll

#ifndef CXXBRIDGE1_STRUCT_PsbtResult
#define CXXBRIDGE1_STRUCT_PsbtResult
// Result wrapper for unsigned transactions.
struct PsbtResult final : public ::rust::Opaque {
  // Check if result is valid.
  bool is_ok() const noexcept;

  // Get error message (only valid if !is_ok()).
  ::rust::String get_psbt_error() const noexcept;

  // Get transaction ID preview (only valid if is_ok()).
  ::rust::String get_txid_preview() const noexcept;

  ~PsbtResult() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_PsbtResult

#ifndef CXXBRIDGE1_STRUCT_NotificationReceiver
#define CXXBRIDGE1_STRUCT_NotificationReceiver
// Notification receiver for blocking recv in a dedicated thread.
struct NotificationReceiver final : public ::rust::Opaque {
  // Blocking receive — waits for the next notification.
  // Returns Poll with is_some()=false when the channel disconnects.
  ::rust::Box<::Poll> recv() const noexcept;

  ~NotificationReceiver() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_NotificationReceiver

#ifndef CXXBRIDGE1_STRUCT_SyncEstimator
#define CXXBRIDGE1_STRUCT_SyncEstimator
// EMA-based sync time estimator.
struct SyncEstimator final : public ::rust::Opaque {
  // Update estimator state from a ScanProgress notification.
  void update(::std::uint32_t current, ::std::uint32_t end) noexcept;

  // Get estimated remaining seconds (0 if no estimate yet).
  ::std::uint64_t estimate_secs() const noexcept;

  // Reset estimator state (on scan start/stop).
  void reset() noexcept;

  ~SyncEstimator() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_SyncEstimator

#ifndef CXXBRIDGE1_STRUCT_HwiService
#define CXXBRIDGE1_STRUCT_HwiService
// Hardware wallet device service.
struct HwiService final : public ::rust::Opaque {
  // Start polling for devices. Idempotent.
  void start() noexcept;

  // Stop polling. Idempotent.
  void stop() noexcept;

  // Switch networks in place, keeping the notification channel. Does not block
  // on the old listener, so it is safe to call from the GUI thread.
  void set_network(::Network network) noexcept;

  // Stop for good and end the notification stream.
  void shutdown() noexcept;

  bool is_running() const noexcept;

  // Take the receiver for a dedicated notification thread. Call once.
  ::rust::Box<::HwiReceiver> take_receiver() noexcept;

  // Every device the service can see, in whatever state.
  ::rust::Vec<::HwiDevice> devices() const noexcept;

  // Requests. Each returns whether it was dispatched; the result arrives on the
  // notification carrying the same req_id.
  bool get_xpub(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str path) const noexcept;

  bool is_descriptor_registered(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str descriptor) const noexcept;
  bool register_descriptor(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str descriptor) const noexcept;
  bool sign_psbt(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str psbt_base64) const noexcept;
  ~HwiService() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_HwiService

#ifndef CXXBRIDGE1_STRUCT_HwiReceiver
#define CXXBRIDGE1_STRUCT_HwiReceiver
// Notification receiver for the HWI service.
struct HwiReceiver final : public ::rust::Opaque {
  // Blocking receive. Returns a poll with is_some()=false once shut down.
  ::rust::Box<::HwiPoll> recv() const noexcept;

  ~HwiReceiver() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_HwiReceiver

#ifndef CXXBRIDGE1_STRUCT_HwiPoll
#define CXXBRIDGE1_STRUCT_HwiPoll
// Poll result for HWI notifications.
struct HwiPoll final : public ::rust::Opaque {
  bool is_some() const noexcept;
  ::HwiEvent get_event() const noexcept;
  ~HwiPoll() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_HwiPoll

// Initialize logging with the specified level.
// Call this once at application startup.
void init_logging(::LogLevel level) noexcept;

// Generate a new 12-word BIP39 mnemonic.
::rust::String generate_mnemonic() noexcept;

::rust::String notification_to_string(::Notification const &notif) noexcept;

// Query backend server info (network, height, capabilities).
// Blocking HTTP call - may take up to 30s on timeout.
::BackendInfo get_backend_info(::rust::String blindbit_url) noexcept;

// Validate a BIP39 mnemonic string.
bool validate_mnemonic(::rust::String mnemonic) noexcept;

// Validate a recipient address (SP address, legacy Bitcoin address, or hex data).
// Returns empty string if valid, or error message if invalid.
::rust::String validate_address(::rust::String address) noexcept;

// Test Electrum server connectivity.
// Blocking call - attempts TCP connect and server.version handshake.
::ConnectionResult test_electrum(::rust::String address) noexcept;

// Fetch default regtest infrastructure addresses from minta API.
// Blocking HTTP call - should be called from a background thread.
::RegtestDefaults get_regtest_defaults() noexcept;

// List enabled app-level plugin IDs.
::rust::Vec<::rust::String> app_enabled_plugins() noexcept;

// Enable or disable a plugin ID at app level.
void app_set_plugin_enabled(::rust::String id, bool enabled) noexcept;

// Get the active app theme name.
::rust::String app_active_theme() noexcept;

// Set the active app theme name.
void app_set_active_theme(::rust::String name) noexcept;

// Create a new config.
::rust::Box<::Config> new_config(::rust::String account_name, ::Network network, ::rust::String mnemonic, ::rust::String blindbit_url, ::rust::String electrum_url, ::std::uint64_t dust_limit, ::rust::String plugin_id) noexcept;

// Load config from file.
::rust::Box<::Config> config_from_file(::rust::String account_name) noexcept;

// List all existing configs.
::rust::Vec<::rust::String> list_configs() noexcept;

// Delete an account's data from disk.
bool delete_config(::rust::String account_name) noexcept;

// Create a new account from an account name (loads config from file).
// Check is_ok() before use; get_error() for failure reason.
::rust::Box<::Account> new_account(::rust::String account_name) noexcept;

// Create the device service. Does not start polling.
::rust::Box<::HwiService> new_hwi_service(::Network network) noexcept;

// Network the device service runs on, persisted in the module's config file.
::Network hwi_config_network() noexcept;

void hwi_set_config_network(::Network network) noexcept;

// Create a new SyncEstimator with default EMA alpha.
::rust::Box<::SyncEstimator> new_sync_estimator() noexcept;

#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__
