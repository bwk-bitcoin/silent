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

enum class KeyFileFailure : ::std::uint8_t;
enum class PluginMetadataKind : ::std::uint8_t;
enum class BackendErrorKind : ::std::uint8_t;
enum class EndpointPhase : ::std::uint8_t;
enum class Network : ::std::uint8_t;
enum class BackendServer : ::std::uint8_t;
enum class LogLevel : ::std::uint8_t;
enum class NotificationFlag : ::std::uint8_t;
enum class HwiDeviceState : ::std::uint8_t;
enum class HwiEventFlag : ::std::uint8_t;
enum class HwiErrorKind : ::std::uint8_t;
enum class KeyRouteKind : ::std::uint8_t;
enum class KeyUnusableReason : ::std::uint8_t;
enum class QrResponseKind : ::std::uint8_t;
enum class MusigRound : ::std::uint8_t;
enum class QrErrorKind : ::std::uint8_t;
struct RetryDecision;
struct FeedQuote;
struct FeedPrice;
struct FeedSelection;
struct FeedSourceState;
struct PluginMetadataField;
struct PluginMetadata;
struct PluginGrantedAccess;
struct AmountParseResult;
struct FeeParseResult;
struct SendAmounts;
struct SpCreationRequest;
struct MusigCreationRequest;
struct AccountCreationResult;
struct RustFeePreferences;
struct Notification;
struct RustCoin;
enum class SpendLockKind : ::std::uint8_t;
enum class SpendMaturity : ::std::uint8_t;
enum class SpendAvailability : ::std::uint8_t;
enum class SpendCoinStatus : ::std::uint8_t;
struct RustSpendLock;
struct RustSpendVariant;
struct RustSpendCoin;
struct SpendCoinsResult;
enum class SigningStage : ::std::uint8_t;
enum class SigningProgress : ::std::uint8_t;
enum class SpendError : ::std::uint8_t;
struct RustSelectedInput;
struct CoordinationResult;
struct CoinState;
struct RustTx;
struct RustTxIo;
struct LabelEditResult;
struct RustTxDetails;
struct EndpointResult;
struct Output;
struct TransactionTemplate;
struct TransactionSimulation;
struct TxResult;
struct MusigFinalizeResult;
struct MusigAttemptSnapshot;
struct SavedPsbtAttempt;
struct SavedPsbtReset;
struct SavedPsbtRecord;
struct SavedPsbtInput;
struct SavedPsbtEntry;
struct SavedPsbtList;
struct SavedPsbtRestore;
struct SavedPsbtMutation;
struct SpDescriptorResult;
struct QrPayload;
struct BbqrScanResult;
struct QrImageResult;
struct ElectrumTipResult;
struct ConnectionResult;
struct RegtestDefaults;
struct PsbtValidation;
struct BackendInfo;
struct HwiDevice;
struct HwiEvent;
struct KeyInfo;
struct KeyDevice;
struct LiveDevice;
struct KeyXpub;
struct AddressVerifyInfo;
struct KeyDescriptor;
struct KeyOpResult;
struct QrMessageResult;
struct KeyInfoResult;
struct SigningCandidate;
struct MusigSessionCreateResult;
struct HotCachedNonceResult;
struct LedgerNonceSignResult;
struct SigningPlan;
struct FingerprintsResult;
struct MnemonicResult;
struct SignResult;
struct HotXpubResult;
struct HotSpDescriptorResult;
struct PsbtPayloadResult;
struct CameraListResult;
struct CameraFrameResult;
struct QrDisplayLevels;
struct StateUnlockInfo;
struct PluginInstallResult;
struct PluginCatalog;
struct PluginLibraryResult;
struct StateIndexResult;
enum class PluginGrant : ::std::uint8_t;
struct PluginAccess;
struct PsbtFileResult;
struct DescriptorBackupResult;
struct MusigDescriptorResult;
struct MusigElectrumUrlResult;
struct MusigCosignerStatus;
struct MusigRoundStatus;
struct MusigSpendLeaf;
struct MusigSpendLeaves;
struct MusigReceiveAddress;
enum class PolicyKeyType : ::std::uint8_t;
enum class PolicySemantic : ::std::uint8_t;
enum class PolicyLocktime : ::std::uint8_t;
enum class PolicyDurationUnit : ::std::uint8_t;
enum class PolicyDraftError : ::std::uint8_t;
struct PolicyDraft;
struct PolicyDraftResult;
struct PolicyKey;
struct PolicyPath;
struct PolicySpec;
struct PolicyCompileResult;
struct PolicyReviewKey;
struct PolicyReviewPath;
struct PolicyReview;
struct Config;
struct Account;
struct Poll;
struct PsbtResult;
struct NotificationReceiver;
struct SyncEstimator;
struct HwiService;
struct HwiReceiver;
struct HwiPoll;
struct KeysModule;
struct QrResponse;
struct HotMaterial;
struct FeedState;
struct EndpointRetry;
struct SpEndpointChange;
struct MusigEndpointChange;
struct KeyFilePreview;
struct KeyFileCommit;
struct SealedHotKey;
struct CameraHandle;
struct BbqrFrames;
struct BbqrScanner;
struct MusigAccount;
struct LedgerNonceFetch;

#ifndef CXXBRIDGE1_ENUM_KeyFileFailure
#define CXXBRIDGE1_ENUM_KeyFileFailure
enum class KeyFileFailure : ::std::uint8_t {
  None = 0,
  Read = 1,
  Newer = 2,
  Network = 3,
  Invalid = 4,
};
#endif // CXXBRIDGE1_ENUM_KeyFileFailure

#ifndef CXXBRIDGE1_ENUM_PluginMetadataKind
#define CXXBRIDGE1_ENUM_PluginMetadataKind
enum class PluginMetadataKind : ::std::uint8_t {
  Text = 0,
  Number = 1,
  Flag = 2,
  Strings = 3,
};
#endif // CXXBRIDGE1_ENUM_PluginMetadataKind

#ifndef CXXBRIDGE1_ENUM_BackendErrorKind
#define CXXBRIDGE1_ENUM_BackendErrorKind
enum class BackendErrorKind : ::std::uint8_t {
  Other = 0,
  None = 1,
  InvalidInput = 2,
  Storage = 3,
  Network = 4,
  Electrum = 5,
  Backend = 6,
  Signing = 7,
  Broadcast = 8,
  InputConflict = 9,
  AddressReuse = 10,
};
#endif // CXXBRIDGE1_ENUM_BackendErrorKind

#ifndef CXXBRIDGE1_ENUM_EndpointPhase
#define CXXBRIDGE1_ENUM_EndpointPhase
enum class EndpointPhase : ::std::uint8_t {
  Ready = 0,
  Draining = 1,
  Complete = 2,
};
#endif // CXXBRIDGE1_ENUM_EndpointPhase

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

#ifndef CXXBRIDGE1_ENUM_BackendServer
#define CXXBRIDGE1_ENUM_BackendServer
// A backend server an account connects to.
enum class BackendServer : ::std::uint8_t {
  Blindbit = 0,
  Electrum = 1,
};
#endif // CXXBRIDGE1_ENUM_BackendServer

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
  AddressDisplayed = 6,
  Error = 7,
};
#endif // CXXBRIDGE1_ENUM_HwiEventFlag

#ifndef CXXBRIDGE1_ENUM_HwiErrorKind
#define CXXBRIDGE1_ENUM_HwiErrorKind
// Why a device request failed, beyond its message.
enum class HwiErrorKind : ::std::uint8_t {
  Other = 0,
  // The device has no MuSig2 session for the PSBT: restart from round 1.
  MusigSession = 1,
};
#endif // CXXBRIDGE1_ENUM_HwiErrorKind

#ifndef CXXBRIDGE1_ENUM_KeyRouteKind
#define CXXBRIDGE1_ENUM_KeyRouteKind
// How a key can be reached, in priority order. Hot always outranks the others.
// `Unknown` never appears in `add_route`/`remove_route`/`set_route_order` calls;
// it only shows up in a `SigningCandidate.route` for a fingerprint with no
// configured route, where `can_sign` is false.
enum class KeyRouteKind : ::std::uint8_t {
  Hot = 0,
  Usb = 1,
  Qr = 2,
  SdCard = 3,
  Unknown = 4,
};
#endif // CXXBRIDGE1_ENUM_KeyRouteKind

#ifndef CXXBRIDGE1_ENUM_KeyUnusableReason
#define CXXBRIDGE1_ENUM_KeyUnusableReason
// Why a key cannot sign the PSBT in hand. `None` means it can.
enum class KeyUnusableReason : ::std::uint8_t {
  None = 0,
  NotRegistered = 1,
  NoRoute = 2,
  DeviceNotConnected = 3,
  NoHotSecret = 4,
  SpOutputsPending = 5,
  UnsupportedMusig = 6,
  UnsupportedOperation = 7,
};
#endif // CXXBRIDGE1_ENUM_KeyUnusableReason

#ifndef CXXBRIDGE1_ENUM_QrResponseKind
#define CXXBRIDGE1_ENUM_QrResponseKind
// Kind of a decoded and validated QR response.
enum class QrResponseKind : ::std::uint8_t {
  Xpubs = 0,
  Registration = 1,
  AddressVerified = 2,
  SignedPsbt = 3,
  SpAccount = 4,
};
#endif // CXXBRIDGE1_ENUM_QrResponseKind

#ifndef CXXBRIDGE1_ENUM_MusigRound
#define CXXBRIDGE1_ENUM_MusigRound
// MuSig2 signing round of a PSBT.
enum class MusigRound : ::std::uint8_t {
  NotMusig = 0,
  Nonces = 1,
  PartialSigs = 2,
  Ready = 3,
};
#endif // CXXBRIDGE1_ENUM_MusigRound

#ifndef CXXBRIDGE1_ENUM_QrErrorKind
#define CXXBRIDGE1_ENUM_QrErrorKind
// Why a QR response was refused: the three failures the UI words on their own
// (a request id mismatch, a signatures-only answer, a lost MuSig2 session), and
// every other one, which only surfaces as free text.
enum class QrErrorKind : ::std::uint8_t {
  Other = 0,
  RequestIdMismatch = 1,
  SignaturesUnsupported = 2,
  // The device lost or refused its MuSig2 session (response error 0x0D).
  MusigSession = 3,
};
#endif // CXXBRIDGE1_ENUM_QrErrorKind

#ifndef CXXBRIDGE1_STRUCT_RetryDecision
#define CXXBRIDGE1_STRUCT_RetryDecision
struct RetryDecision final {
  bool schedule CXX_DEFAULT_VALUE(false);
  bool give_up CXX_DEFAULT_VALUE(false);
  ::std::uint32_t losses CXX_DEFAULT_VALUE(0);
  ::std::uint32_t delay_ms CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RetryDecision

#ifndef CXXBRIDGE1_STRUCT_FeedQuote
#define CXXBRIDGE1_STRUCT_FeedQuote
struct FeedQuote final {
  bool available CXX_DEFAULT_VALUE(false);
  double value CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_FeedQuote

#ifndef CXXBRIDGE1_STRUCT_FeedPrice
#define CXXBRIDGE1_STRUCT_FeedPrice
struct FeedPrice final {
  ::rust::String source;
  ::rust::String currency;
  ::FeedQuote quote;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_FeedPrice

#ifndef CXXBRIDGE1_STRUCT_RustFeePreferences
#define CXXBRIDGE1_STRUCT_RustFeePreferences
// Global fee mode (Manual=0, Default=1, Smart=2) and tier (Low=0, Medium=1, Fast=2).
struct RustFeePreferences final {
  ::std::uint8_t mode CXX_DEFAULT_VALUE(0);
  ::rust::String default_source;
  ::rust::String smart_source;
  ::std::uint8_t tier CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustFeePreferences

#ifndef CXXBRIDGE1_STRUCT_FeedSelection
#define CXXBRIDGE1_STRUCT_FeedSelection
struct FeedSelection final {
  ::RustFeePreferences preferences;
  ::FeedQuote default_rate;
  ::FeedQuote low;
  ::FeedQuote medium;
  ::FeedQuote fast;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_FeedSelection

#ifndef CXXBRIDGE1_STRUCT_FeedSourceState
#define CXXBRIDGE1_STRUCT_FeedSourceState
struct FeedSourceState final {
  ::rust::Vec<::rust::String> currencies;
  bool has_default CXX_DEFAULT_VALUE(false);
  bool has_smart CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_FeedSourceState

#ifndef CXXBRIDGE1_STRUCT_PluginMetadataField
#define CXXBRIDGE1_STRUCT_PluginMetadataField
struct PluginMetadataField final {
  ::rust::String key;
  ::PluginMetadataKind kind;
  ::rust::String text;
  double number CXX_DEFAULT_VALUE(0);
  bool flag CXX_DEFAULT_VALUE(false);
  ::rust::Vec<::rust::String> items;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PluginMetadataField

#ifndef CXXBRIDGE1_STRUCT_PluginMetadata
#define CXXBRIDGE1_STRUCT_PluginMetadata
struct PluginMetadata final {
  ::rust::String id;
  ::rust::Vec<::PluginMetadataField> fields;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PluginMetadata

#ifndef CXXBRIDGE1_STRUCT_PluginGrantedAccess
#define CXXBRIDGE1_STRUCT_PluginGrantedAccess
struct PluginGrantedAccess final {
  bool account CXX_DEFAULT_VALUE(false);
  bool keys CXX_DEFAULT_VALUE(false);
  bool state CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PluginGrantedAccess

#ifndef CXXBRIDGE1_STRUCT_AmountParseResult
#define CXXBRIDGE1_STRUCT_AmountParseResult
struct AmountParseResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::std::uint64_t sats CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_AmountParseResult

#ifndef CXXBRIDGE1_STRUCT_FeeParseResult
#define CXXBRIDGE1_STRUCT_FeeParseResult
struct FeeParseResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::std::uint64_t fee CXX_DEFAULT_VALUE(0);
  double fee_rate CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_FeeParseResult

#ifndef CXXBRIDGE1_STRUCT_SendAmounts
#define CXXBRIDGE1_STRUCT_SendAmounts
struct SendAmounts final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::std::uint64_t input_total CXX_DEFAULT_VALUE(0);
  ::std::uint64_t sending_total CXX_DEFAULT_VALUE(0);
  ::std::uint64_t remaining CXX_DEFAULT_VALUE(0);
  ::std::uint64_t change CXX_DEFAULT_VALUE(0);
  ::std::uint64_t transaction_total CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SendAmounts

#ifndef CXXBRIDGE1_STRUCT_SpCreationRequest
#define CXXBRIDGE1_STRUCT_SpCreationRequest
struct SpCreationRequest final {
  ::rust::String account_name;
  ::Network network;
  ::rust::String descriptor;
  ::rust::String taproot_descriptor;
  ::rust::String segwit_descriptor;
  ::rust::String blindbit_url;
  ::rust::String electrum_url;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SpCreationRequest

#ifndef CXXBRIDGE1_STRUCT_MusigCreationRequest
#define CXXBRIDGE1_STRUCT_MusigCreationRequest
struct MusigCreationRequest final {
  ::rust::String account_name;
  ::Network network;
  ::rust::String descriptor;
  ::rust::String electrum_url;
  bool verify_certificate CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigCreationRequest

#ifndef CXXBRIDGE1_STRUCT_AccountCreationResult
#define CXXBRIDGE1_STRUCT_AccountCreationResult
struct AccountCreationResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String preference_error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_AccountCreationResult

#ifndef CXXBRIDGE1_STRUCT_Notification
#define CXXBRIDGE1_STRUCT_Notification
// Notification from the account scanner.
struct Notification final {
  ::NotificationFlag flag;
  ::rust::String payload;
  ::std::uint32_t current CXX_DEFAULT_VALUE(0);
  ::std::uint32_t end CXX_DEFAULT_VALUE(0);

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
  // Confirming block time, zero until known.
  ::std::uint64_t timestamp CXX_DEFAULT_VALUE(0);
  ::rust::String label;
  bool spent CXX_DEFAULT_VALUE(false);
  ::rust::String account_type;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustCoin

#ifndef CXXBRIDGE1_ENUM_SpendLockKind
#define CXXBRIDGE1_ENUM_SpendLockKind
enum class SpendLockKind : ::std::uint8_t {
  None = 0,
  AbsoluteHeight = 1,
  AbsoluteTime = 2,
  RelativeBlocks = 3,
  RelativeTime512 = 4,
};
#endif // CXXBRIDGE1_ENUM_SpendLockKind

#ifndef CXXBRIDGE1_ENUM_SpendMaturity
#define CXXBRIDGE1_ENUM_SpendMaturity
enum class SpendMaturity : ::std::uint8_t {
  Mature = 0,
  Immature = 1,
  Unknown = 2,
};
#endif // CXXBRIDGE1_ENUM_SpendMaturity

#ifndef CXXBRIDGE1_ENUM_SpendAvailability
#define CXXBRIDGE1_ENUM_SpendAvailability
enum class SpendAvailability : ::std::uint8_t {
  Available = 0,
  Immature = 1,
  Unknown = 2,
  Spent = 3,
};
#endif // CXXBRIDGE1_ENUM_SpendAvailability

#ifndef CXXBRIDGE1_ENUM_SpendCoinStatus
#define CXXBRIDGE1_ENUM_SpendCoinStatus
enum class SpendCoinStatus : ::std::uint8_t {
  Unconfirmed = 0,
  ConfirmedUnverified = 1,
  Confirmed = 2,
  BeingSpent = 3,
  Spent = 4,
};
#endif // CXXBRIDGE1_ENUM_SpendCoinStatus

#ifndef CXXBRIDGE1_STRUCT_RustSpendLock
#define CXXBRIDGE1_STRUCT_RustSpendLock
struct RustSpendLock final {
  ::SpendLockKind kind;
  ::std::uint32_t value CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustSpendLock

#ifndef CXXBRIDGE1_STRUCT_RustSpendVariant
#define CXXBRIDGE1_STRUCT_RustSpendVariant
struct RustSpendVariant final {
  ::rust::String path_id;
  ::std::uint32_t keychain CXX_DEFAULT_VALUE(0);
  ::std::uint32_t derivation_index CXX_DEFAULT_VALUE(0);
  ::rust::String display;
  ::rust::String psbt_v2_hex;
  ::rust::String descriptor;
  ::rust::String sp_spend_key;
  ::rust::Vec<::rust::String> required_keys;
  ::rust::Vec<::rust::String> required_preimages;
  ::std::uint64_t satisfaction_wu CXX_DEFAULT_VALUE(0);
  ::RustSpendLock absolute_lock;
  ::RustSpendLock relative_lock;
  ::SpendMaturity maturity;
  ::SpendAvailability availability;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustSpendVariant

#ifndef CXXBRIDGE1_STRUCT_RustSpendCoin
#define CXXBRIDGE1_STRUCT_RustSpendCoin
struct RustSpendCoin final {
  ::rust::String outpoint;
  ::rust::String account_type;
  ::Network network;
  ::std::uint64_t value CXX_DEFAULT_VALUE(0);
  ::std::uint64_t height CXX_DEFAULT_VALUE(0);
  bool has_height CXX_DEFAULT_VALUE(false);
  ::rust::String label;
  ::SpendCoinStatus status;
  ::rust::Vec<::RustSpendVariant> variants;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustSpendCoin

#ifndef CXXBRIDGE1_STRUCT_SpendCoinsResult
#define CXXBRIDGE1_STRUCT_SpendCoinsResult
struct SpendCoinsResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::Vec<::RustSpendCoin> coins;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SpendCoinsResult

#ifndef CXXBRIDGE1_ENUM_SigningStage
#define CXXBRIDGE1_ENUM_SigningStage
enum class SigningStage : ::std::uint8_t {
  Signatures = 0,
  MusigNonces = 1,
  MusigSignatures = 2,
  SpContributions = 3,
  VerifySpContributions = 4,
  CompleteSpOutputs = 5,
  SpCompleteAndSign = 6,
  Release = 7,
};
#endif // CXXBRIDGE1_ENUM_SigningStage

#ifndef CXXBRIDGE1_ENUM_SigningProgress
#define CXXBRIDGE1_ENUM_SigningProgress
enum class SigningProgress : ::std::uint8_t {
  Partial = 0,
  StageComplete = 1,
  Complete = 2,
};
#endif // CXXBRIDGE1_ENUM_SigningProgress

#ifndef CXXBRIDGE1_ENUM_SpendError
#define CXXBRIDGE1_ENUM_SpendError
enum class SpendError : ::std::uint8_t {
  Other = 0,
  Unsupported = 1,
  InvalidRequest = 2,
  Integrity = 3,
  LostSession = 4,
  Permission = 5,
  Cancelled = 6,
};
#endif // CXXBRIDGE1_ENUM_SpendError

#ifndef CXXBRIDGE1_STRUCT_RustSelectedInput
#define CXXBRIDGE1_STRUCT_RustSelectedInput
struct RustSelectedInput final {
  ::std::uint32_t input_index CXX_DEFAULT_VALUE(0);
  ::Network network;
  ::RustSpendVariant variant;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustSelectedInput

#ifndef CXXBRIDGE1_STRUCT_CoordinationResult
#define CXXBRIDGE1_STRUCT_CoordinationResult
struct CoordinationResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::SpendError kind;
  ::rust::String psbt_v2_hex;
  ::SigningProgress progress;
  ::rust::String tx_hex;
  ::rust::String txid;
  ::rust::String session_id;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_CoordinationResult

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
  bool amount_known CXX_DEFAULT_VALUE(false);
  ::std::uint64_t fee CXX_DEFAULT_VALUE(0);
  bool fee_known CXX_DEFAULT_VALUE(false);
  ::std::uint32_t height CXX_DEFAULT_VALUE(0);
  ::std::uint64_t timestamp CXX_DEFAULT_VALUE(0);
  ::rust::String label;
  ::rust::String status;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustTx

#ifndef CXXBRIDGE1_STRUCT_RustTxIo
#define CXXBRIDGE1_STRUCT_RustTxIo
struct RustTxIo final {
  ::std::uint32_t index CXX_DEFAULT_VALUE(0);
  ::rust::String outpoint;
  ::std::uint64_t value CXX_DEFAULT_VALUE(0);
  bool value_known CXX_DEFAULT_VALUE(false);
  ::rust::String address;
  ::rust::String script;
  bool is_mine CXX_DEFAULT_VALUE(false);
  bool ownership_known CXX_DEFAULT_VALUE(false);
  ::rust::String label;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustTxIo

#ifndef CXXBRIDGE1_STRUCT_LabelEditResult
#define CXXBRIDGE1_STRUCT_LabelEditResult
struct LabelEditResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_LabelEditResult

#ifndef CXXBRIDGE1_STRUCT_RustTxDetails
#define CXXBRIDGE1_STRUCT_RustTxDetails
struct RustTxDetails final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  // Missing prevouts or an offline lookup do not invalidate known transaction data.
  ::rust::String warning;
  ::RustTx tx;
  ::Network network;
  ::std::int32_t version CXX_DEFAULT_VALUE(0);
  ::std::uint32_t lock_time CXX_DEFAULT_VALUE(0);
  ::std::uint64_t weight CXX_DEFAULT_VALUE(0);
  ::std::uint64_t vsize CXX_DEFAULT_VALUE(0);
  ::rust::Vec<::RustTxIo> inputs;
  ::rust::Vec<::RustTxIo> outputs;
  ::std::uint64_t input_total CXX_DEFAULT_VALUE(0);
  bool input_total_known CXX_DEFAULT_VALUE(false);
  ::std::uint64_t output_total CXX_DEFAULT_VALUE(0);
  ::std::uint64_t fee CXX_DEFAULT_VALUE(0);
  bool fee_known CXX_DEFAULT_VALUE(false);
  double fee_rate CXX_DEFAULT_VALUE(0);
  ::std::uint32_t chain_tip CXX_DEFAULT_VALUE(0);
  bool chain_tip_known CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_RustTxDetails

#ifndef CXXBRIDGE1_STRUCT_EndpointResult
#define CXXBRIDGE1_STRUCT_EndpointResult
struct EndpointResult final {
  ::BackendErrorKind kind;
  // Reopen succeeded; transport confirmation still arrives through notifications.
  bool connection_pending CXX_DEFAULT_VALUE(false);
  bool is_ok CXX_DEFAULT_VALUE(false);
  bool saved CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String url;
  bool verify_certificate CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_EndpointResult

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
  ::BackendErrorKind kind;
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
  ::rust::Vec<::std::uint64_t> selected_values;
  ::rust::Vec<::std::uint64_t> recipient_amounts;
  ::std::uint64_t sending_total CXX_DEFAULT_VALUE(0);
  ::std::uint64_t change CXX_DEFAULT_VALUE(0);
  ::std::uint64_t transaction_total CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_TransactionSimulation

#ifndef CXXBRIDGE1_STRUCT_TxResult
#define CXXBRIDGE1_STRUCT_TxResult
// Result of a transaction operation (sign, broadcast).
struct TxResult final {
  ::BackendErrorKind kind;
  // Whether the operation succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // Result value: signed tx hex or txid (empty on error).
  ::rust::String value;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_TxResult

#ifndef CXXBRIDGE1_STRUCT_MusigFinalizeResult
#define CXXBRIDGE1_STRUCT_MusigFinalizeResult
struct MusigFinalizeResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String psbt_v2_hex;
  ::rust::String tx_hex;
  // Compatibility with callers that used TxResult::value.
  ::rust::String value;
  ::rust::String save_error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigFinalizeResult

#ifndef CXXBRIDGE1_STRUCT_MusigAttemptSnapshot
#define CXXBRIDGE1_STRUCT_MusigAttemptSnapshot
struct MusigAttemptSnapshot final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String save_error;
  ::rust::String finalize_error;
  ::rust::String psbt_v2_hex;
  ::rust::String tx_hex;
  ::rust::String record_id;
  ::rust::Vec<::rust::String> signers;
  bool reset_pending CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigAttemptSnapshot

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtAttempt
#define CXXBRIDGE1_STRUCT_SavedPsbtAttempt
struct SavedPsbtAttempt final {
  ::std::uint64_t attempt_id CXX_DEFAULT_VALUE(0);
  ::rust::String baseline_psbt_v2_hex;
  ::rust::String current_psbt_v2_hex;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtAttempt

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtReset
#define CXXBRIDGE1_STRUCT_SavedPsbtReset
struct SavedPsbtReset final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String save_error;
  ::rust::String psbt_v2_hex;
  bool reset_pending CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtReset

#ifndef CXXBRIDGE1_STRUCT_MusigSpendLeaf
#define CXXBRIDGE1_STRUCT_MusigSpendLeaf
// One way to spend a musig account's coins: the key path or one tap leaf
// (bwk `SpendLeaf`). Picked before round 1 (decision 15).
struct MusigSpendLeaf final {
  // The key path (internal key); leaf_hash is empty.
  bool key_path CXX_DEFAULT_VALUE(false);
  // Hex tap leaf hash.
  ::rust::String leaf_hash;
  // Master fingerprints of the keys that sign it.
  ::rust::Vec<::rust::String> fingerprints;
  // `older()` blocks, 0 for none.
  ::std::uint32_t relative CXX_DEFAULT_VALUE(0);
  // `after()` height, 0 for none.
  ::std::uint32_t absolute CXX_DEFAULT_VALUE(0);
  // Every given coin's timelock on it is met at the current tip.
  bool spendable CXX_DEFAULT_VALUE(false);
  // Witness weight of one input spent this way.
  ::std::uint64_t satisfaction_wu CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigSpendLeaf

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtRecord
#define CXXBRIDGE1_STRUCT_SavedPsbtRecord
struct SavedPsbtRecord final {
  ::rust::String id;
  ::TransactionTemplate tx_template;
  ::MusigSpendLeaf spend;
  ::rust::String original_psbt_v2_hex;
  ::rust::String current_psbt_v2_hex;
  ::std::uint64_t attempt_id CXX_DEFAULT_VALUE(0);
  ::rust::String baseline_psbt_v2_hex;
  ::rust::Vec<::SavedPsbtAttempt> archived_attempts;
  bool reset_pending CXX_DEFAULT_VALUE(false);
  ::rust::String finalized_tx_hex;
  bool broadcast CXX_DEFAULT_VALUE(false);
  ::rust::String label;
  ::std::uint64_t created_at CXX_DEFAULT_VALUE(0);
  ::std::uint64_t updated_at CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtRecord

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtInput
#define CXXBRIDGE1_STRUCT_SavedPsbtInput
struct SavedPsbtInput final {
  ::rust::String outpoint;
  ::std::uint64_t value CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtInput

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtEntry
#define CXXBRIDGE1_STRUCT_SavedPsbtEntry
struct SavedPsbtEntry final {
  ::SavedPsbtRecord record;
  ::rust::String txid;
  ::TransactionTemplate frozen_template;
  ::rust::Vec<::SavedPsbtInput> inputs;
  ::std::uint64_t input_total CXX_DEFAULT_VALUE(0);
  ::std::uint64_t output_total CXX_DEFAULT_VALUE(0);
  ::std::uint32_t confirmed_height CXX_DEFAULT_VALUE(0);
  bool in_history CXX_DEFAULT_VALUE(false);
  ::rust::Vec<::rust::String> conflicting_txids;
  ::rust::String warning;
  ::std::uint32_t signatures_complete CXX_DEFAULT_VALUE(0);
  ::std::uint32_t signatures_required CXX_DEFAULT_VALUE(0);
  bool signatures_known CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtEntry

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtList
#define CXXBRIDGE1_STRUCT_SavedPsbtList
struct SavedPsbtList final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String save_error;
  ::rust::Vec<::SavedPsbtEntry> entries;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtList

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtRestore
#define CXXBRIDGE1_STRUCT_SavedPsbtRestore
struct SavedPsbtRestore final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String save_error;
  ::SavedPsbtEntry entry;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtRestore

#ifndef CXXBRIDGE1_STRUCT_SavedPsbtMutation
#define CXXBRIDGE1_STRUCT_SavedPsbtMutation
struct SavedPsbtMutation final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String save_error;
  ::rust::String record_id;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SavedPsbtMutation

#ifndef CXXBRIDGE1_STRUCT_SpDescriptorResult
#define CXXBRIDGE1_STRUCT_SpDescriptorResult
// Result of decoding an SP descriptor QR response.
struct SpDescriptorResult final {
  // Whether the decode succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // Decoded SP descriptor (empty on error).
  ::rust::String descriptor;
  // Origin fingerprint of the sp() descriptor (empty on error).
  ::rust::String fingerprint;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SpDescriptorResult

#ifndef CXXBRIDGE1_STRUCT_QrPayload
#define CXXBRIDGE1_STRUCT_QrPayload
// One payload a QR scan joined, its bytes as framed.
struct QrPayload final {
  ::rust::Vec<::std::uint8_t> data;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_QrPayload

#ifndef CXXBRIDGE1_STRUCT_BbqrScanResult
#define CXXBRIDGE1_STRUCT_BbqrScanResult
// What one camera frame gave a BBQR scan: the payloads it completed, and the parts
// seen of the total of the payload being joined (0 of 0 while none is).
struct BbqrScanResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::Vec<::QrPayload> payloads;
  ::std::uint32_t seen CXX_DEFAULT_VALUE(0);
  ::std::uint32_t total CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_BbqrScanResult

#ifndef CXXBRIDGE1_STRUCT_QrImageResult
#define CXXBRIDGE1_STRUCT_QrImageResult
// Grayscale QR image for the C++ UI.
struct QrImageResult final {
  // Whether rendering succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // One byte per pixel: 0 is black, 255 is white.
  ::rust::Vec<::std::uint8_t> data;
  ::std::uint32_t width CXX_DEFAULT_VALUE(0);
  ::std::uint32_t height CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_QrImageResult

#ifndef CXXBRIDGE1_STRUCT_ElectrumTipResult
#define CXXBRIDGE1_STRUCT_ElectrumTipResult
// The chain tip height an Electrum server follows.
struct ElectrumTipResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  ::std::uint32_t height CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_ElectrumTipResult

#ifndef CXXBRIDGE1_STRUCT_ConnectionResult
#define CXXBRIDGE1_STRUCT_ConnectionResult
// Result of a connection test.
struct ConnectionResult final {
  ::BackendErrorKind kind;
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
  ::BackendErrorKind kind;
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
  // Capability bits: 0 ordinary, 1 Taproot, 2 silent payments, 3 MuSig2. Zero
  // unless the device is Supported.
  ::std::uint32_t capabilities CXX_DEFAULT_VALUE(0);
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
  // Error events only, `Other` otherwise.
  ::HwiErrorKind error_kind;
  // WalletRegistered only: the device's proof, hex, empty when it returned none.
  ::rust::String hmac;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_HwiEvent

#ifndef CXXBRIDGE1_STRUCT_KeyInfo
#define CXXBRIDGE1_STRUCT_KeyInfo
// A registered key as the Keys tab shows it.
struct KeyInfo final {
  ::rust::String fingerprint;
  ::rust::String alias;
  bool has_hot_secret CXX_DEFAULT_VALUE(false);
  // In priority order, first entry is the preferred route.
  ::rust::Vec<::KeyRouteKind> routes;
  bool is_connected CXX_DEFAULT_VALUE(false);
  ::std::uint32_t device_count CXX_DEFAULT_VALUE(0);
  ::std::uint32_t descriptor_count CXX_DEFAULT_VALUE(0);
  // Signs MuSig2: a hot secret, or a recorded or live device reporting the
  // MuSig2 capability.
  bool can_musig CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_KeyInfo

#ifndef CXXBRIDGE1_STRUCT_KeyDevice
#define CXXBRIDGE1_STRUCT_KeyDevice
// A device remembered for a key. `model` and `serial` are empty until bwk-hwi
// reports them; they exist only to deduplicate devices sharing a fingerprint.
// `device_id` and `brand` are always empty: `DeviceRecord` carries neither a
// stable id nor a brand today.
struct KeyDevice final {
  ::rust::String fingerprint;
  ::rust::String device_id;
  ::rust::String kind;
  ::rust::String brand;
  ::rust::String model;
  ::rust::String serial;
  ::rust::String module;
  // Firmware version, empty for a USB device.
  ::rust::String version;
  // Last reported bwk-qr capability bits, including ordinary/Taproot for USB.
  ::std::uint32_t capabilities CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_KeyDevice

#ifndef CXXBRIDGE1_STRUCT_LiveDevice
#define CXXBRIDGE1_STRUCT_LiveDevice
// A device the signer bus can see right now, fed in from the hwi module.
// `brand` is always empty: `manager::LiveDevice` carries no brand field
// because bwk-hwi has not reported one yet.
struct LiveDevice final {
  ::rust::String device_id;
  ::rust::String fingerprint;
  ::rust::String kind;
  ::rust::String brand;
  ::rust::String model;
  ::rust::String serial;
  ::rust::String module;
  // Firmware version, empty for a USB device.
  ::rust::String version;
  // Explicit ordinary, Taproot, silent payments and MuSig2 capability bits.
  ::std::uint32_t capabilities CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_LiveDevice

#ifndef CXXBRIDGE1_STRUCT_KeyXpub
#define CXXBRIDGE1_STRUCT_KeyXpub
// A cached extended public key at a derivation path for a registered key.
struct KeyXpub final {
  ::rust::String fingerprint;
  ::rust::String path;
  ::rust::String xpub;
  // An account or a registered descriptor carried it, even one since deleted.
  bool used CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_KeyXpub

#ifndef CXXBRIDGE1_STRUCT_AddressVerifyInfo
#define CXXBRIDGE1_STRUCT_AddressVerifyInfo
// Descriptor and derivation path needed to verify a generated address on a
// signing device. Empty when `address` cannot be resolved (e.g. the silent
// payment address itself, which is not derived at a BIP32 path).
struct AddressVerifyInfo final {
  ::rust::String descriptor;
  ::rust::String path;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_AddressVerifyInfo

#ifndef CXXBRIDGE1_STRUCT_KeyDescriptor
#define CXXBRIDGE1_STRUCT_KeyDescriptor
// A descriptor a key participates in.
struct KeyDescriptor final {
  ::rust::String fingerprint;
  ::rust::String descriptor;
  // The sha256-prefix alias the device knows this descriptor by.
  ::rust::String alias;
  bool registered CXX_DEFAULT_VALUE(false);
  // The proof the device returned for the registration, hex, empty when none.
  ::rust::String hmac;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_KeyDescriptor

#ifndef CXXBRIDGE1_STRUCT_KeyOpResult
#define CXXBRIDGE1_STRUCT_KeyOpResult
// Result of a keys module operation carrying no response data.
struct KeyOpResult final {
  // Whether the operation succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_KeyOpResult

#ifndef CXXBRIDGE1_STRUCT_QrMessageResult
#define CXXBRIDGE1_STRUCT_QrMessageResult
// A QR protocol message, as the bytes a transport frames.
struct QrMessageResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  ::rust::Vec<::std::uint8_t> payload;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_QrMessageResult

#ifndef CXXBRIDGE1_STRUCT_KeyInfoResult
#define CXXBRIDGE1_STRUCT_KeyInfoResult
// Result of a keys module operation carrying a `KeyInfo`.
struct KeyInfoResult final {
  // Whether the operation succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // The key info (empty/zeroed fields on error).
  ::KeyInfo key;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_KeyInfoResult

#ifndef CXXBRIDGE1_STRUCT_SigningCandidate
#define CXXBRIDGE1_STRUCT_SigningCandidate
// One row of the signer picker. The picker is PSBT driven: every fingerprint the
// PSBT needs gets an entry. Connected devices unrelated to the PSBT are not listed
// here; the C++ picker derives those hints from its live signers.
struct SigningCandidate final {
  ::rust::String fingerprint;
  ::rust::String alias;
  // Meaningless when `can_sign` is false; `Unknown` is the sentinel value.
  ::KeyRouteKind route;
  // Set only when `route` is Usb: the id C++ passes to the device's own ISigner.
  ::rust::String device_id;
  bool can_sign CXX_DEFAULT_VALUE(false);
  // Why not, when `can_sign` is false. `None` otherwise.
  ::KeyUnusableReason unusable;
  // The SP key, which completes the output scripts before anyone else signs.
  bool must_sign_first CXX_DEFAULT_VALUE(false);
  bool registered CXX_DEFAULT_VALUE(false);
  // The key already gave its share for the PSBT's current MuSig2 round.
  bool musig_done CXX_DEFAULT_VALUE(false);
  bool nonces_done CXX_DEFAULT_VALUE(false);
  bool signatures_done CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SigningCandidate

#ifndef CXXBRIDGE1_STRUCT_MusigSessionCreateResult
#define CXXBRIDGE1_STRUCT_MusigSessionCreateResult
struct MusigSessionCreateResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  bool wrong_password CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigSessionCreateResult

#ifndef CXXBRIDGE1_STRUCT_HotCachedNonceResult
#define CXXBRIDGE1_STRUCT_HotCachedNonceResult
struct HotCachedNonceResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  bool needs_password CXX_DEFAULT_VALUE(false);
  bool musig_session CXX_DEFAULT_VALUE(false);
  ::rust::String psbt_v2_hex;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_HotCachedNonceResult

#ifndef CXXBRIDGE1_STRUCT_LedgerNonceSignResult
#define CXXBRIDGE1_STRUCT_LedgerNonceSignResult
struct LedgerNonceSignResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  bool musig_session CXX_DEFAULT_VALUE(false);
  bool need_fetch CXX_DEFAULT_VALUE(false);
  bool musig CXX_DEFAULT_VALUE(false);
  bool from_cache CXX_DEFAULT_VALUE(false);
  ::rust::String psbt_v2_hex;
  ::rust::String psbt_base64;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_LedgerNonceSignResult

#ifndef CXXBRIDGE1_STRUCT_SigningPlan
#define CXXBRIDGE1_STRUCT_SigningPlan
// Signer picker for one PSBT, built by `KeysModule::signing_plan`.
struct SigningPlan final {
  // Whether the plan could be built at all (false on a malformed PSBT, for
  // example). Unrelated to whether any candidate `can_sign`.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // Ordered: `must_sign_first` first, then usable, then unusable.
  ::rust::Vec<::SigningCandidate> candidates;
  // An output script is still underived, so only the SP key may sign now.
  bool sp_outputs_pending CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SigningPlan

#ifndef CXXBRIDGE1_STRUCT_FingerprintsResult
#define CXXBRIDGE1_STRUCT_FingerprintsResult
// Result of a fingerprint lookup carrying no key/module state.
struct FingerprintsResult final {
  // Whether the lookup succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  ::rust::Vec<::rust::String> fingerprints;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_FingerprintsResult

#ifndef CXXBRIDGE1_STRUCT_MnemonicResult
#define CXXBRIDGE1_STRUCT_MnemonicResult
// Result of a mnemonic operation (generation or reveal).
struct MnemonicResult final {
  // Whether the operation succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // Empty on error. Never holds a partial or placeholder phrase.
  ::rust::String mnemonic;
  // The password did not open the hot secret. Only `hot_reveal` sets it.
  bool wrong_password CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MnemonicResult

#ifndef CXXBRIDGE1_STRUCT_SignResult
#define CXXBRIDGE1_STRUCT_SignResult
// Result of a hot signing operation.
struct SignResult final {
  // Whether the signing operation succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // The password did not open the hot secret. Only `hot_sign` sets it.
  bool wrong_password CXX_DEFAULT_VALUE(false);
  // The hot key has no MuSig2 session for this PSBT: restart from round 1.
  bool musig_session CXX_DEFAULT_VALUE(false);
  // The signed PSBT, serialized v2 and hex encoded. Empty on error.
  ::rust::String psbt_v2_hex;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_SignResult

#ifndef CXXBRIDGE1_STRUCT_HotXpubResult
#define CXXBRIDGE1_STRUCT_HotXpubResult
// Result of `hot_sp_descriptor`.
struct HotXpubResult final {
  // Whether the xpub was derived.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // The password did not open the hot secret.
  bool wrong_password CXX_DEFAULT_VALUE(false);
  // The xpub at the asked path. Empty on error.
  ::rust::String xpub;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_HotXpubResult

#ifndef CXXBRIDGE1_STRUCT_HotSpDescriptorResult
#define CXXBRIDGE1_STRUCT_HotSpDescriptorResult
struct HotSpDescriptorResult final {
  // Whether the descriptor was derived.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // The password did not open the hot secret.
  bool wrong_password CXX_DEFAULT_VALUE(false);
  // The sp() descriptor, origin included. Empty on error.
  ::rust::String descriptor;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_HotSpDescriptorResult

#ifndef CXXBRIDGE1_STRUCT_PsbtPayloadResult
#define CXXBRIDGE1_STRUCT_PsbtPayloadResult
// Result of converting a PSBT v2 into its v0 base64 payload for a USB signing
// device.
struct PsbtPayloadResult final {
  // Whether the conversion succeeded.
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Error message (empty if is_ok is true).
  ::rust::String error;
  // The v0 PSBT, base64 encoded. Empty on error.
  ::rust::String psbt_base64;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PsbtPayloadResult

#ifndef CXXBRIDGE1_STRUCT_CameraListResult
#define CXXBRIDGE1_STRUCT_CameraListResult
struct CameraListResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::Vec<::rust::String> names;
  // Parallel to `names`: the index to pass to camera_open.
  ::rust::Vec<::std::uint32_t> indexes;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_CameraListResult

#ifndef CXXBRIDGE1_STRUCT_CameraFrameResult
#define CXXBRIDGE1_STRUCT_CameraFrameResult
// Three bytes per pixel, interleaved RGB888.
struct CameraFrameResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::Vec<::std::uint8_t> data;
  ::std::uint32_t width CXX_DEFAULT_VALUE(0);
  ::std::uint32_t height CXX_DEFAULT_VALUE(0);
  // Monotonic. C++ repaints only when it changes.
  ::std::uint64_t seq CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_CameraFrameResult

#ifndef CXXBRIDGE1_STRUCT_QrDisplayLevels
#define CXXBRIDGE1_STRUCT_QrDisplayLevels
// The QR display steppers' levels, 1 lowest to 5 highest.
struct QrDisplayLevels final {
  ::std::uint8_t density CXX_DEFAULT_VALUE(0);
  ::std::uint8_t contrast CXX_DEFAULT_VALUE(0);
  ::std::uint8_t speed CXX_DEFAULT_VALUE(0);

  bool operator==(QrDisplayLevels const &) const noexcept;
  bool operator!=(QrDisplayLevels const &) const noexcept;
  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_QrDisplayLevels

#ifndef CXXBRIDGE1_STRUCT_StateUnlockInfo
#define CXXBRIDGE1_STRUCT_StateUnlockInfo
// How the app state unlocks.
struct StateUnlockInfo final {
  // The state has an unlock key: false before it is created.
  bool exists CXX_DEFAULT_VALUE(false);
  // A signing device, else a password.
  bool device CXX_DEFAULT_VALUE(false);
  // The path to ask the device for its key at, "m/138'/1936225652'/<index>'".
  ::rust::String path;
  // Set when the check file cannot be read.
  ::rust::String error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_StateUnlockInfo

#ifndef CXXBRIDGE1_STRUCT_PluginInstallResult
#define CXXBRIDGE1_STRUCT_PluginInstallResult
// What installing a dropped plugin gave.
struct PluginInstallResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String plugin_id;
  ::rust::String error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PluginInstallResult

#ifndef CXXBRIDGE1_STRUCT_PluginCatalog
#define CXXBRIDGE1_STRUCT_PluginCatalog
// One translation catalog of a plugin, by file name (`silent_en.lang`).
struct PluginCatalog final {
  ::rust::String name;
  ::rust::String text;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PluginCatalog

#ifndef CXXBRIDGE1_STRUCT_PluginLibraryResult
#define CXXBRIDGE1_STRUCT_PluginLibraryResult
// An installed plugin's library and catalogs, opened from the state.
struct PluginLibraryResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  // The library's file name, for its type.
  ::rust::String library_name;
  ::rust::Vec<::std::uint8_t> library;
  ::rust::Vec<::PluginCatalog> catalogs;
  ::rust::String error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PluginLibraryResult

#ifndef CXXBRIDGE1_STRUCT_StateIndexResult
#define CXXBRIDGE1_STRUCT_StateIndexResult
// A device key's index, as `state_draw_index` answers it.
struct StateIndexResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  // Below 2^31, set only when `is_ok`.
  ::std::uint32_t index CXX_DEFAULT_VALUE(0);
  ::rust::String error;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_StateIndexResult

#ifndef CXXBRIDGE1_ENUM_PluginGrant
#define CXXBRIDGE1_ENUM_PluginGrant
// The user's answer to one use an external plugin asked for.
enum class PluginGrant : ::std::uint8_t {
  // Not asked yet: the host asks on the plugin's first use.
  Unasked = 0,
  Allowed = 1,
  Denied = 2,
};
#endif // CXXBRIDGE1_ENUM_PluginGrant

#ifndef CXXBRIDGE1_STRUCT_PluginAccess
#define CXXBRIDGE1_STRUCT_PluginAccess
// What one external plugin library may use, by its sha256.
struct PluginAccess final {
  ::rust::String plugin_id;
  // The sha256 of the plugin library, hex.
  ::rust::String hash;
  // May call `IAccount`, on every account.
  ::PluginGrant account;
  // May call `IKeys`.
  ::PluginGrant keys;
  // May open the plugin's own saved state.
  ::PluginGrant state;

  bool operator==(PluginAccess const &) const noexcept;
  bool operator!=(PluginAccess const &) const noexcept;
  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PluginAccess

#ifndef CXXBRIDGE1_STRUCT_PsbtFileResult
#define CXXBRIDGE1_STRUCT_PsbtFileResult
struct PsbtFileResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  // The PSBT read from disk, hex encoded. Empty on save and on error.
  ::rust::String psbt_hex;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PsbtFileResult

#ifndef CXXBRIDGE1_STRUCT_DescriptorBackupResult
#define CXXBRIDGE1_STRUCT_DescriptorBackupResult
// Result of writing a descriptor's BIP138 backup.
struct DescriptorBackupResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  // The key expressions left out of the encryption keys: their holders cannot
  // decrypt the backup with them.
  ::rust::Vec<::rust::String> excluded;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_DescriptorBackupResult

#ifndef CXXBRIDGE1_STRUCT_MusigDescriptorResult
#define CXXBRIDGE1_STRUCT_MusigDescriptorResult
// Result of validating a musig() descriptor.
struct MusigDescriptorResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  // Standard BIP-390 text with checksum. Empty on error.
  ::rust::String descriptor;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigDescriptorResult

#ifndef CXXBRIDGE1_STRUCT_MusigElectrumUrlResult
#define CXXBRIDGE1_STRUCT_MusigElectrumUrlResult
// Result of saving the Electrum endpoint of a MuSig2 account.
struct MusigElectrumUrlResult final {
  // The url was written to the config.
  bool saved CXX_DEFAULT_VALUE(false);
  // Kept for existing callers. Saving never starts a listener.
  bool started CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigElectrumUrlResult

#ifndef CXXBRIDGE1_STRUCT_MusigCosignerStatus
#define CXXBRIDGE1_STRUCT_MusigCosignerStatus
// Signing progress of one cosigner on a PSBT.
struct MusigCosignerStatus final {
  ::rust::String fingerprint;
  ::std::uint32_t nonces CXX_DEFAULT_VALUE(0);
  ::std::uint32_t partial_sigs CXX_DEFAULT_VALUE(0);
  ::std::uint32_t expected CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigCosignerStatus

#ifndef CXXBRIDGE1_STRUCT_MusigRoundStatus
#define CXXBRIDGE1_STRUCT_MusigRoundStatus
// Signing progress of a PSBT.
struct MusigRoundStatus final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::MusigRound round;
  ::rust::Vec<::MusigCosignerStatus> cosigners;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigRoundStatus

#ifndef CXXBRIDGE1_STRUCT_MusigSpendLeaves
#define CXXBRIDGE1_STRUCT_MusigSpendLeaves
struct MusigSpendLeaves final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  // Key path first, then leaves in DFS order.
  ::rust::Vec<::MusigSpendLeaf> leaves;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigSpendLeaves

#ifndef CXXBRIDGE1_STRUCT_MusigReceiveAddress
#define CXXBRIDGE1_STRUCT_MusigReceiveAddress
// First receive address of a musig() descriptor, before any account holds it,
// with what IKeys::verifyAddress needs (the AddressVerifyInfo of that address).
struct MusigReceiveAddress final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::String address;
  ::rust::String descriptor;
  ::rust::String path;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_MusigReceiveAddress

#ifndef CXXBRIDGE1_ENUM_PolicyKeyType
#define CXXBRIDGE1_ENUM_PolicyKeyType
// Role of a key (bwk-bup `KeyType`), stored in its origin's last step R.
enum class PolicyKeyType : ::std::uint8_t {
  Internal = 0,
  External = 1,
  Cosigner = 2,
  SafetyNet = 3,
  // Review only: the origin carries no known R.
  Unknown = 4,
};
#endif // CXXBRIDGE1_ENUM_PolicyKeyType

#ifndef CXXBRIDGE1_ENUM_PolicySemantic
#define CXXBRIDGE1_ENUM_PolicySemantic
// Shape of a path's keys (bwk-bup `Semantic`).
enum class PolicySemantic : ::std::uint8_t {
  Single = 0,
  // m-of-n; every multisig compiles to MuSig2 leaves.
  Multi = 1,
  // Mandatory keys plus a subset of the others.
  MultiMandatory = 2,
  // Review only: nested, custom or unknown shapes.
  Other = 3,
};
#endif // CXXBRIDGE1_ENUM_PolicySemantic

#ifndef CXXBRIDGE1_ENUM_PolicyLocktime
#define CXXBRIDGE1_ENUM_PolicyLocktime
// Timelock kind of a path (bwk-bup `Locktime`).
enum class PolicyLocktime : ::std::uint8_t {
  None = 0,
  Relative = 1,
  AbsoluteRenewable = 2,
  Absolute = 3,
};
#endif // CXXBRIDGE1_ENUM_PolicyLocktime

#ifndef CXXBRIDGE1_ENUM_PolicyDurationUnit
#define CXXBRIDGE1_ENUM_PolicyDurationUnit
enum class PolicyDurationUnit : ::std::uint8_t {
  Blocks = 0,
  Days = 1,
  Weeks = 2,
  Months = 3,
};
#endif // CXXBRIDGE1_ENUM_PolicyDurationUnit

#ifndef CXXBRIDGE1_ENUM_PolicyDraftError
#define CXXBRIDGE1_ENUM_PolicyDraftError
enum class PolicyDraftError : ::std::uint8_t {
  None = 0,
  Count = 1,
  Date = 2,
  Future = 3,
  Tip = 4,
  RelativeMax = 5,
  Bounds = 6,
};
#endif // CXXBRIDGE1_ENUM_PolicyDraftError

#ifndef CXXBRIDGE1_STRUCT_PolicyDraft
#define CXXBRIDGE1_STRUCT_PolicyDraft
struct PolicyDraft final {
  ::rust::Vec<::rust::String> keys;
  ::rust::Vec<::rust::String> mandatory;
  ::std::uint32_t threshold CXX_DEFAULT_VALUE(0);
  ::PolicyLocktime locktime;
  ::std::uint32_t count CXX_DEFAULT_VALUE(0);
  ::PolicyDurationUnit unit;
  ::std::int64_t date_timestamp CXX_DEFAULT_VALUE(0);
  bool date_valid CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyDraft

#ifndef CXXBRIDGE1_STRUCT_PolicyPath
#define CXXBRIDGE1_STRUCT_PolicyPath
struct PolicyPath final {
  ::PolicySemantic semantic;
  // Signers per spend: 1 for Single, m for Multi, mandatory plus cosigners
  // for MultiMandatory.
  ::std::uint32_t threshold CXX_DEFAULT_VALUE(0);
  ::PolicyLocktime locktime;
  // Blocks for Relative, height for the absolute kinds, 0 for None.
  ::std::uint32_t locktime_value CXX_DEFAULT_VALUE(0);
  ::rust::Vec<::PolicyKey> keys;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyPath

#ifndef CXXBRIDGE1_STRUCT_PolicyDraftResult
#define CXXBRIDGE1_STRUCT_PolicyDraftResult
struct PolicyDraftResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::PolicyDraftError lock_error;
  ::PolicyPath path;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyDraftResult

#ifndef CXXBRIDGE1_STRUCT_PolicyKey
#define CXXBRIDGE1_STRUCT_PolicyKey
// One key of a path, as the creator gathered it. The role key
// `[fp/87'/coin'/account'/R]xpub` is its role xpub, fetched at R 0 (Internal).
struct PolicyKey final {
  ::rust::String fingerprint;
  // The BIP-87 account path `m/87'/coin'/account'`.
  ::rust::String account_path;
  // The xpub at `account_path`/0, the Internal role's; ignored by preview_policy.
  ::rust::String role_xpub;
  ::PolicyKeyType key_type;
  // MultiMandatory only: the key signs every leaf.
  bool mandatory CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyKey

#ifndef CXXBRIDGE1_STRUCT_PolicySpec
#define CXXBRIDGE1_STRUCT_PolicySpec
// A policy from the creator. The binding infers the bwk-bup `PolicyType` from
// the paths: PrimaryOnly (one locktime-free path, no recovery), Csv, Cltv,
// CsvWithMandatoryKey, Unknown (an absolute timelock beside mandatory keys) or
// Invalid (mixed timelock kinds, or several paths and none timelocked).
struct PolicySpec final {
  ::Network network;
  ::rust::Vec<::PolicyPath> paths;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicySpec

#ifndef CXXBRIDGE1_STRUCT_PolicyCompileResult
#define CXXBRIDGE1_STRUCT_PolicyCompileResult
struct PolicyCompileResult final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  // Index in PolicySpec.paths the error is about, -1 for the whole policy.
  ::std::int32_t error_path CXX_DEFAULT_VALUE(0);
  // The error is PolicyError::TooManyLeaves.
  bool too_many_leaves CXX_DEFAULT_VALUE(false);
  // Standard BIP-390 text with checksum; empty on error and from preview_policy.
  ::rust::String descriptor;
  // Tap leaves, padding excluded.
  ::std::uint32_t leaf_count CXX_DEFAULT_VALUE(0);
  // bwk-bup MAX_LEAVES.
  ::std::uint32_t max_leaves CXX_DEFAULT_VALUE(0);
  // The primary path is the internal key (key path spend).
  bool has_key_path CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyCompileResult

#ifndef CXXBRIDGE1_STRUCT_PolicyReviewKey
#define CXXBRIDGE1_STRUCT_PolicyReviewKey
struct PolicyReviewKey final {
  ::rust::String fingerprint;
  // Origin path, `m/87'/coin'/account'/R` form.
  ::rust::String origin_path;
  ::rust::String xpub;
  ::PolicyKeyType key_type;
  bool mandatory CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyReviewKey

#ifndef CXXBRIDGE1_STRUCT_PolicyReviewPath
#define CXXBRIDGE1_STRUCT_PolicyReviewPath
struct PolicyReviewPath final {
  ::PolicySemantic semantic;
  ::std::uint32_t threshold CXX_DEFAULT_VALUE(0);
  ::PolicyLocktime locktime;
  ::std::uint32_t locktime_value CXX_DEFAULT_VALUE(0);
  ::rust::Vec<::PolicyReviewKey> keys;
  // Its tap leaves; 0 on the key path.
  ::std::uint32_t leaf_count CXX_DEFAULT_VALUE(0);
  bool key_path CXX_DEFAULT_VALUE(false);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyReviewPath

#ifndef CXXBRIDGE1_STRUCT_PolicyReview
#define CXXBRIDGE1_STRUCT_PolicyReview
// A descriptor lifted back to paths and key types, the C++ side writes the
// plain words from it.
struct PolicyReview final {
  bool is_ok CXX_DEFAULT_VALUE(false);
  ::rust::String error;
  ::rust::Vec<::PolicyReviewPath> paths;
  ::std::uint32_t leaf_count CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_PolicyReview

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

  // Empty on success, otherwise the write error.
  ::rust::String save() const noexcept;

  // Get SP descriptor.
  ::rust::String get_descriptor() const noexcept;

  // Set SP descriptor.
  void set_descriptor(::rust::String descriptor) noexcept;

  // The segwit sub-account descriptor, empty for none.
  ::rust::String get_segwit_descriptor() const noexcept;

  // Set the segwit sub-account descriptor, empty for none.
  void set_segwit_descriptor(::rust::String descriptor) noexcept;

  // The taproot sub-account descriptor, empty for none.
  ::rust::String get_taproot_descriptor() const noexcept;

  // Set the taproot sub-account descriptor, empty for none.
  void set_taproot_descriptor(::rust::String descriptor) noexcept;

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

  // Whether the Electrum server's TLS certificate is checked.
  bool get_verify_certificate() const noexcept;

  // Check the Electrum server's TLS certificate, or not (a self-signed server).
  void set_verify_certificate(bool verify) noexcept;

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

  // The account's network (Regtest when it failed to load).
  ::Network network() const noexcept;

  ::rust::String electrum_url() const noexcept;
  ::rust::String blindbit_url() const noexcept;
  bool verify_certificate() const noexcept;
  bool electrum_running() const noexcept;

  // Whether the account has a segwit (wpkh) sub-account.
  bool has_segwit() const noexcept;

  // Whether the account has a taproot (tr) sub-account.
  bool has_taproot() const noexcept;

  // Generate a new segwit (wpkh) receiving address.
  ::rust::String new_segwit_addr() noexcept;

  // Generate a new taproot receiving address.
  ::rust::String new_taproot_addr() noexcept;

  // Get all coins (spent and unspent).
  ::rust::Vec<::RustCoin> coins() const noexcept;

  ::SpendCoinsResult spending_coins() const noexcept;

  // Get spendable coins summary.
  ::CoinState spendable_coins() const noexcept;

  // Update a coin's label.
  ::LabelEditResult update_coin_label(::rust::String outpoint, ::rust::String label) const noexcept;

  // Get payment history.
  ::rust::Vec<::RustTx> payment_history() const noexcept;

  ::EndpointResult reopen_backend(bool connect_electrum) noexcept;
  void close_backend() noexcept;
  ::rust::String shutdown_backend() noexcept;
  ::RustTxDetails tx_details(::rust::Str txid, bool allow_network) const noexcept;
  ::LabelEditResult update_tx_label(::rust::String txid, ::rust::String label) const noexcept;

  // Get the account SP descriptor.
  ::rust::String descriptor() const noexcept;

  // Descriptor and derivation path to verify `address` on a signing device.
  ::AddressVerifyInfo address_verify_info(::rust::Str address) const noexcept;

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

  // Finalize a signed PSBT and return transaction hex.
  ::TxResult finalize_psbt(::rust::String signed_psbt_base64) const noexcept;

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

  // Get the serialized PSBT as hex (only valid if is_ok()).
  ::rust::String bytes_hex() const noexcept;

  // Get the serialized PSBT v2 as hex (only valid if is_ok()).
  ::rust::String bytes_v2_hex() const noexcept;

  ::rust::String saved_record_id() const noexcept;
  ::rust::String save_error() const noexcept;
  ::BackendErrorKind get_error_kind() const noexcept;
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

  // Like get_xpub, shown on the device: a Ledger gives a non-standard path only so.
  bool get_xpub_shown(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str path) const noexcept;

  bool is_descriptor_registered(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str descriptor) const noexcept;
  bool register_descriptor(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str descriptor) const noexcept;

  // Signs for the registered wallet of `descriptor`. `hmac` is the stored proof
  // of the descriptor on this key, hex, empty when none.
  bool sign_psbt(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str psbt_base64, ::rust::Str descriptor, ::rust::Str hmac) const noexcept;

  // Asks the device to show the address at `path`. A single key BIP-86 path is
  // shown directly, any other descriptor is shown as the registered wallet's
  // address at `change/index`. `hmac` is the stored proof of the descriptor on
  // this key, hex, empty when none.
  bool display_address(::std::uint64_t req_id, ::rust::Str device_id, ::rust::Str descriptor, ::rust::Str path, ::rust::Str hmac) const noexcept;

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

#ifndef CXXBRIDGE1_STRUCT_KeysModule
#define CXXBRIDGE1_STRUCT_KeysModule
// Owns the key manager for one network: records, routes, devices, the xpub
// cache and registered descriptors.
struct KeysModule final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;

  // Reloads the records for `network`. The Keys tab combobox drives this.
  // On failure the previously-loaded manager is left untouched; only the
  // error returned by `get_error()` changes.
  ::KeyOpResult set_network(::Network network) noexcept;

  // Returns `Regtest` as a sentinel if the module has no inner manager.
  // Callers MUST check `is_ok()` before trusting this value.
  ::Network network() const noexcept;

  ::rust::Vec<::KeyInfo> keys() const noexcept;
  ::KeyInfoResult key(::rust::Str fingerprint) const noexcept;
  ::KeyInfoResult add_key(::rust::Str fingerprint, ::rust::Str alias) noexcept;

  // Refuses while descriptors are registered unless `force` is set; the UI sets
  // it only after warning.
  ::KeyOpResult remove_key(::rust::Str fingerprint, bool force) noexcept;

  ::KeyOpResult set_alias(::rust::Str fingerprint, ::rust::Str alias) noexcept;
  ::KeyOpResult add_route(::rust::Str fingerprint, ::KeyRouteKind kind) noexcept;
  ::KeyOpResult remove_route(::rust::Str fingerprint, ::KeyRouteKind kind) noexcept;

  // `order` is the full route list, highest priority first.
  ::KeyOpResult set_route_order(::rust::Str fingerprint, ::rust::Vec<::KeyRouteKind> order) noexcept;

  // Replaces the live list wholesale; the C++ side calls this on every
  // devicesChanged from the hwi module.
  void set_live_devices(::rust::Vec<::LiveDevice> devices) noexcept;

  ::KeyOpResult check_device_signing(::rust::Str psbt_v2_hex, ::rust::Str fingerprint, ::std::uint32_t capabilities) const noexcept;
  ::rust::Vec<::LiveDevice> live_devices() const noexcept;

  // Remembers a device for the key with that fingerprint.
  ::KeyOpResult record_device(::LiveDevice device) noexcept;

  ::rust::Vec<::KeyDevice> key_devices(::rust::Str fingerprint) const noexcept;
  ::KeyOpResult cache_xpub(::rust::Str fingerprint, ::rust::Str path, ::rust::Str xpub) noexcept;

  // Drops a cached xpub that is not used and that no registered descriptor
  // carries.
  ::KeyOpResult remove_xpub(::rust::Str fingerprint, ::rust::Str path) noexcept;

  ::rust::Vec<::KeyXpub> key_xpubs(::rust::Str fingerprint) const noexcept;

  // Marks used every key xpub the accounts of the network are built on, caching
  // it when the key's cache lacks it.
  ::KeyOpResult mark_account_xpubs_used() noexcept;

  ::KeyOpResult register_descriptor(::rust::Str fingerprint, ::rust::Str descriptor) noexcept;

  // Records `descriptor` as registered with the key, with the proof the device
  // returned (`hmac`, 32 bytes of hex).
  ::KeyOpResult set_descriptor_hmac(::rust::Str fingerprint, ::rust::Str descriptor, ::rust::Str hmac) noexcept;

  ::KeyOpResult unregister_descriptor(::rust::Str fingerprint, ::rust::Str descriptor) noexcept;
  ::rust::Vec<::KeyDescriptor> key_descriptors(::rust::Str fingerprint) const noexcept;

  // Every registered key whose descriptor list contains this descriptor.
  ::rust::Vec<::KeyInfo> keys_for_descriptor(::rust::Str descriptor) const noexcept;

  // Builds the signer picker for this PSBT. Blocks on nothing; safe to call
  // from the GUI thread.
  ::SigningPlan signing_plan(::rust::Str psbt_v2_hex) const noexcept;

  // Copies out the record and the manager's network for the free hot-key
  // worker functions below, so a `set_network` or route change afterward
  // cannot race a worker already in flight against the copy. Fails for an
  // unknown key or a key without a secret.
  ::rust::Box<::HotMaterial> hot_material(::rust::Str fingerprint) const noexcept;

  ::KeyOpResult apply_key_file(::KeyFileCommit const &committed) noexcept;

  // Commits a key `hot_seal` built into the store, adding the Hot route in
  // the same update. Refuses a sealed key whose network does not match the
  // manager's.
  ::KeyInfoResult commit_hot_key(::SealedHotKey const &sealed, ::rust::Str alias) noexcept;

  ::QrMessageResult qr_request_get_xpubs(::rust::Vec<::rust::String> paths) noexcept;
  ::QrMessageResult qr_request_register_descriptor(::rust::Str descriptor) noexcept;
  ::QrMessageResult qr_request_verify_address(::rust::Str descriptor, ::rust::Str path, ::rust::Str address) noexcept;
  ::QrMessageResult qr_request_sign(::rust::Vec<::rust::String> descriptors, ::rust::Str psbt_v2_hex) noexcept;
  ::QrMessageResult qr_request_get_sp_account() noexcept;

  // Decodes a response's bytes and validates them against the outstanding
  // request.
  ::rust::Box<::QrResponse> qr_decode_response(::rust::Slice<::std::uint8_t const> payload) noexcept;

  ~KeysModule() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_KeysModule

#ifndef CXXBRIDGE1_STRUCT_QrResponse
#define CXXBRIDGE1_STRUCT_QrResponse
// A decoded and validated QR response.
struct QrResponse final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;

  // Only meaningful when !is_ok().
  ::QrErrorKind error_kind() const noexcept;

  // Only meaningful when is_ok().
  ::QrResponseKind kind() const noexcept;

  // The signer's fingerprint when kind() is Xpubs or SpAccount, else empty.
  ::rust::String fingerprint() const noexcept;

  // The SP descriptor when kind() is SpAccount, else empty.
  ::rust::String descriptor() const noexcept;

  // The signed PSBT as v2 hex when kind() is SignedPsbt, else empty.
  ::rust::String psbt_hex() const noexcept;

  // The device vendor when kind() is Xpubs, else empty.
  ::rust::String vendor() const noexcept;

  // The device model when kind() is Xpubs, else empty.
  ::rust::String model() const noexcept;

  // The device firmware version when kind() is Xpubs, else empty.
  ::rust::String version() const noexcept;

  // The device capability bits when kind() is Xpubs, else 0.
  ::std::uint32_t capabilities() const noexcept;

  // The derived xpubs when kind() is Xpubs, the taproot and segwit xpubs when
  // SpAccount, else empty.
  ::rust::Vec<::KeyXpub> xpubs() const noexcept;

  // The device's verify_address answer when kind() is AddressVerified, else false.
  bool verified() const noexcept;

  bool registered() const noexcept;
  ~QrResponse() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_QrResponse

#ifndef CXXBRIDGE1_STRUCT_HotMaterial
#define CXXBRIDGE1_STRUCT_HotMaterial
// GUI-thread copy of one key's record and the manager's network, for the
// free hot-key worker functions to sign/reveal/derive from off-thread.
struct HotMaterial final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;
  ~HotMaterial() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_HotMaterial

#ifndef CXXBRIDGE1_STRUCT_FeedState
#define CXXBRIDGE1_STRUCT_FeedState
struct FeedState final : public ::rust::Opaque {
  void add_feed(::rust::Str id) noexcept;
  void remove_feed(::rust::Str id) noexcept;
  void select_price(::rust::String source, ::rust::String selected_currency) noexcept;
  void set_currencies(::rust::Str id, ::rust::Vec<::rust::String> offered) noexcept;
  void set_price_quote(::rust::Str id, ::rust::Str code, double value) noexcept;
  ::FeedSourceState feed_source(::rust::Str id) const noexcept;
  ::FeedPrice price_selection() const noexcept;
  bool select_fees(::RustFeePreferences preferences) noexcept;
  void set_fee_capabilities(::rust::Str id, bool has_default, bool has_smart) noexcept;
  void set_default_fee(::rust::Str id, double value) noexcept;
  void set_smart_fees(::rust::Str id, double low, double medium, double fast) noexcept;
  ::FeedSelection fee_selection() const noexcept;
  ~FeedState() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_FeedState

#ifndef CXXBRIDGE1_STRUCT_EndpointRetry
#define CXXBRIDGE1_STRUCT_EndpointRetry
struct EndpointRetry final : public ::rust::Opaque {
  void retry_connected() noexcept;
  void retry_stopped() noexcept;
  ::RetryDecision retry_lost(::std::int64_t now_ms) noexcept;
  bool retry_due() noexcept;
  ~EndpointRetry() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_EndpointRetry

#ifndef CXXBRIDGE1_STRUCT_SpEndpointChange
#define CXXBRIDGE1_STRUCT_SpEndpointChange
struct SpEndpointChange final : public ::rust::Opaque {
  ::EndpointPhase endpoint_phase() const noexcept;
  ::BackendServer endpoint_server() const noexcept;
  ::EndpointResult endpoint_result() const noexcept;
  void prepare_endpoint() noexcept;
  void finish_endpoint_after_drain() noexcept;
  ~SpEndpointChange() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_SpEndpointChange

#ifndef CXXBRIDGE1_STRUCT_MusigEndpointChange
#define CXXBRIDGE1_STRUCT_MusigEndpointChange
struct MusigEndpointChange final : public ::rust::Opaque {
  ::EndpointPhase endpoint_phase() const noexcept;
  ::BackendServer endpoint_server() const noexcept;
  ::EndpointResult endpoint_result() const noexcept;
  void prepare_endpoint() noexcept;
  void finish_endpoint_after_drain() noexcept;
  ~MusigEndpointChange() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_MusigEndpointChange

#ifndef CXXBRIDGE1_STRUCT_KeyFilePreview
#define CXXBRIDGE1_STRUCT_KeyFilePreview
struct KeyFilePreview final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;
  ::KeyFileFailure file_failure() const noexcept;
  ::Network file_network() const noexcept;
  ::rust::String file_fingerprint() const noexcept;
  ::rust::String file_alias() const noexcept;
  ::rust::Vec<::KeyXpub> file_xpubs() const noexcept;
  ~KeyFilePreview() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_KeyFilePreview

#ifndef CXXBRIDGE1_STRUCT_KeyFileCommit
#define CXXBRIDGE1_STRUCT_KeyFileCommit
struct KeyFileCommit final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;
  ::BackendErrorKind get_error_kind() const noexcept;
  ~KeyFileCommit() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_KeyFileCommit

#ifndef CXXBRIDGE1_STRUCT_SealedHotKey
#define CXXBRIDGE1_STRUCT_SealedHotKey
// A mnemonic sealed by `hot_seal`, committed by `KeysModule::commit_hot_key`.
struct SealedHotKey final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;

  // Empty on error.
  ::rust::String fingerprint() const noexcept;

  ~SealedHotKey() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_SealedHotKey

#ifndef CXXBRIDGE1_STRUCT_CameraHandle
#define CXXBRIDGE1_STRUCT_CameraHandle
// A running camera, opened by `camera_open`. Dropping it stops the capture.
struct CameraHandle final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;

  // The latest frame, is_ok false until the first one is captured.
  ::CameraFrameResult frame() const noexcept;

  // Why the capture stopped on its own, empty while it runs.
  ::rust::String failure() const noexcept;

  ~CameraHandle() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_CameraHandle

#ifndef CXXBRIDGE1_STRUCT_BbqrFrames
#define CXXBRIDGE1_STRUCT_BbqrFrames
// The BBQR frames of one payload.
struct BbqrFrames final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;
  ::std::uint32_t count() const noexcept;

  // An index past the end answers with is_ok false.
  ::QrImageResult frame(::std::uint32_t index) const noexcept;

  ~BbqrFrames() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_BbqrFrames

#ifndef CXXBRIDGE1_STRUCT_BbqrScanner
#define CXXBRIDGE1_STRUCT_BbqrScanner
// Joins BBQR parts scanned off camera frames back into payloads.
struct BbqrScanner final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;

  // Scans one RGB888 frame of `width * height * 3` bytes.
  ::BbqrScanResult scan(::rust::Slice<::std::uint8_t const> rgb, ::std::uint32_t width, ::std::uint32_t height) noexcept;

  // Drops the parts seen so far.
  void reset() noexcept;

  ~BbqrScanner() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_BbqrScanner

#ifndef CXXBRIDGE1_STRUCT_MusigAccount
#define CXXBRIDGE1_STRUCT_MusigAccount
// A MuSig2 policy account, wrapping bwk::Account.
struct MusigAccount final : public ::rust::Opaque {
  bool is_ok() const noexcept;
  ::rust::String get_error() const noexcept;
  ::rust::String name() const noexcept;

  // Regtest when it failed to load.
  ::Network network() const noexcept;

  // Standard BIP-390 text.
  ::rust::String descriptor() const noexcept;

  ::rust::Vec<::rust::String> cosigner_fingerprints() const noexcept;

  // Reloads the Electrum endpoint from the config and starts the listener.
  bool start_electrum() noexcept;

  void stop_electrum() noexcept;
  ::rust::String electrum_url() const noexcept;

  // Whether the Electrum server's TLS certificate is checked.
  bool verify_certificate() const noexcept;

  // Saves the endpoint and stops the listener. Reopen explicitly to connect.
  ::MusigElectrumUrlResult set_electrum_url(::rust::String url) noexcept;

  ::rust::Box<::Poll> try_recv() noexcept;
  ::rust::Box<::NotificationReceiver> take_receiver() noexcept;
  ::std::uint64_t balance() const noexcept;
  ::rust::String new_addr() noexcept;
  ::rust::Vec<::RustCoin> coins() const noexcept;
  ::SpendCoinsResult spending_coins() const noexcept;
  ::CoinState spendable_coins() const noexcept;
  ::LabelEditResult update_coin_label(::rust::String outpoint, ::rust::String label) const noexcept;
  ::rust::Vec<::RustTx> payment_history() const noexcept;
  ::RustTxDetails tx_details(::rust::Str txid, bool allow_network) const noexcept;
  ::LabelEditResult update_tx_label(::rust::String txid, ::rust::String label) const noexcept;
  ::EndpointResult reopen_backend(bool connect_electrum) noexcept;
  void close_backend() noexcept;
  ::rust::String shutdown_backend() noexcept;
  ::AddressVerifyInfo address_verify_info(::rust::Str address) const noexcept;
  ::TransactionSimulation simulate_transaction(::TransactionTemplate tx_template) const noexcept;

  // Every spend path for the coins `outpoints`, `spendable` at the current tip.
  ::MusigSpendLeaves spend_leaves(::rust::Vec<::rust::String> outpoints) const noexcept;

  // Prepares the PSBT, then restricts it to `spend` (Account::restrict_spend_path).
  ::rust::Box<::PsbtResult> prepare_transaction(::TransactionTemplate tx_template, ::MusigSpendLeaf spend) const noexcept;

  // Per cosigner nonce / partial sig counts of the PSBT.
  ::MusigRoundStatus round_status(::rust::Str psbt_v2_hex) const noexcept;

  // Aggregates partial sigs and returns the finalized PSBT and transaction.
  ::MusigFinalizeResult finalize_psbt(::rust::Str psbt_v2_hex) const noexcept;

  // Disk I/O: call all saved PSBT mutations on the account's worker thread.
  ::SavedPsbtList saved_psbts(bool include_confirmed) const noexcept;

  ::SavedPsbtRestore restore_saved_psbt(::rust::Str id) const noexcept;
  ::SavedPsbtMutation update_saved_psbt(::rust::Str id, ::rust::Str psbt_v2_hex) const noexcept;
  ::SavedPsbtReset reset_saved_psbt(::rust::Str id) const noexcept;
  ::SavedPsbtMutation label_saved_psbt(::rust::Str id, ::rust::String label) const noexcept;
  ::SavedPsbtMutation delete_saved_psbt(::rust::Str id) const noexcept;
  ::SavedPsbtMutation retry_saved_psbts() const noexcept;
  ::rust::String saved_psbt_error() const noexcept;
  ::MusigAttemptSnapshot active_attempt() const noexcept;
  bool clear_active_attempt() const noexcept;
  ::MusigAttemptSnapshot accept_signed_progress(::rust::Str answer) const noexcept;
  ::MusigAttemptSnapshot finalize_active_attempt() const noexcept;
  ::SavedPsbtRestore activate_saved_psbt(::rust::Str id) const noexcept;
  ::MusigAttemptSnapshot reset_active_attempt() const noexcept;
  ::MusigAttemptSnapshot require_active_restart() const noexcept;
  ::SigningPlan active_signing_plan(::rust::Vec<::LiveDevice> devices) const noexcept;

  // TxResult value is the txid.
  ::TxResult broadcast_transaction(::rust::String signed_tx_hex) const noexcept;

  // Blocking: call from a background thread.
  ::PsbtValidation validate_before_sign(::PsbtResult const &psbt_result) const noexcept;

  ~MusigAccount() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_MusigAccount

#ifndef CXXBRIDGE1_STRUCT_LedgerNonceFetch
#define CXXBRIDGE1_STRUCT_LedgerNonceFetch
struct LedgerNonceFetch final : public ::rust::Opaque {
  ::PsbtPayloadResult payload() const noexcept;
  ::PsbtPayloadResult accept(::rust::Str signed_psbt) noexcept;
  ~LedgerNonceFetch() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_LedgerNonceFetch

::CoordinationResult coordination_prepare(::Network network, ::rust::Str psbt, ::rust::Vec<::RustSelectedInput> inputs, ::SigningStage stage) noexcept;

::CoordinationResult coordination_accept(::rust::Str original, ::rust::Str returned, ::rust::Vec<::RustSelectedInput> inputs, ::SigningStage stage) noexcept;

::CoordinationResult coordination_release(::Network network, ::rust::Str psbt, ::rust::Vec<::RustSelectedInput> inputs, ::rust::Str session_id) noexcept;

::CoordinationResult transaction_finalize(::Network network, ::rust::Str psbt, ::rust::Vec<::RustSelectedInput> inputs, ::rust::Str session_id) noexcept;

::CoordinationResult transaction_broadcast(::Network network, ::rust::Str endpoint, ::rust::Str tx_hex) noexcept;

// `m/87'/coin'/account'` for `network`.
::rust::String bip87_account_path(::Network network, ::std::uint32_t account) noexcept;

// `target` rounded up to the grid a renewable absolute timelock sits on.
::std::uint32_t renewable_height(::std::uint32_t target) noexcept;

// Compiles with placeholder keys (role_xpub ignored): leaf count, cap and
// shape errors before any device is asked. descriptor stays empty.
::PolicyCompileResult preview_policy(::PolicySpec spec) noexcept;

// Takes each key's role xpub as its role key, builds the bwk-bup Policy and
// compiles it, every multisig to MuSig2.
::PolicyCompileResult compile_policy(::PolicySpec spec) noexcept;

// `Policy::from_descriptor` of BIP-390 text, key types read from R.
::PolicyReview review_policy(::rust::String descriptor) noexcept;

::rust::Box<::EndpointRetry> new_endpoint_retry() noexcept;

::rust::Box<::SpEndpointChange> sp_startup_endpoint_change(::rust::Box<::Account> account) noexcept;

::rust::Box<::SpEndpointChange> sp_endpoint_change(::rust::Box<::Account> account, ::BackendServer server, ::rust::String url, bool verify, bool test, bool connected) noexcept;

::rust::Box<::MusigEndpointChange> musig_endpoint_change(::rust::Box<::MusigAccount> account, ::rust::String url, bool verify, bool test, bool connected) noexcept;

::rust::Box<::Account> take_sp_endpoint_account(::rust::Box<::SpEndpointChange> change) noexcept;

::rust::Box<::MusigAccount> take_musig_endpoint_account(::rust::Box<::MusigEndpointChange> change) noexcept;

::rust::Box<::FeedState> new_feed_state() noexcept;

// Initialize logging with the specified level.
// Call this once at application startup.
void init_logging(::LogLevel level) noexcept;

::rust::String notification_to_string(::Notification const &notif) noexcept;

// Query backend server info (network, height, capabilities).
// Blocking HTTP call - may take up to 30s on timeout.
::BackendInfo get_backend_info(::rust::String blindbit_url) noexcept;

// Validate a BIP392 Silent Payments descriptor.
bool validate_sp_descriptor(::rust::String descriptor) noexcept;

// Render `text` (an address) as a single QR code.
::QrImageResult qr_text_image(::rust::Str text) noexcept;

// Splits `payload` into BBQR parts of `part_bytes`, one QR image each. Touches
// nothing shared: safe on a worker thread.
::rust::Box<::BbqrFrames> bbqr_frames(::rust::Slice<::std::uint8_t const> payload, ::std::uint32_t part_bytes) noexcept;

::rust::Box<::BbqrScanner> bbqr_scanner_new() noexcept;

// Also reads plain codes: each one's text is a payload, as an address code.
::rust::Box<::BbqrScanner> bbqr_text_scanner_new() noexcept;

// Parses an sp() descriptor and requires an origin. Returns the descriptor
// unchanged plus its fingerprint, or an error saying the origin is missing.
::SpDescriptorResult validate_sp_descriptor_origin(::rust::String descriptor) noexcept;

// Every fingerprint the PSBT needs a signature from, lowercase hex,
// deduplicated and sorted. Walks bip32_derivation, tap_key_origins and the
// silent payment spend derivation.
::FingerprintsResult psbt_required_fingerprints(::rust::Str psbt_v2_hex) noexcept;

// Validate a recipient address (SP address, legacy Bitcoin address, or hex data).
// Returns empty string if valid, or error message if invalid.
::rust::String validate_address(::rust::String address) noexcept;

// Test Electrum server connectivity.
// Blocking call - attempts TCP connect and server.version handshake.
::ConnectionResult test_electrum(::rust::String address, bool verify_certificate) noexcept;

// Test, then persist the complete endpoint. Does not start a wallet listener.
::EndpointResult test_and_save_endpoint(::rust::String account_name, ::BackendServer server, ::rust::String url, bool verify_certificate) noexcept;

// The height of the chain tip the Electrum server at `address` follows.
// Blocking: call it off the GUI thread.
::ElectrumTipResult electrum_tip_height(::rust::String address, bool verify_certificate) noexcept;

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

::RustFeePreferences app_fee_preferences() noexcept;

void app_set_fee_preferences(::RustFeePreferences preferences) noexcept;

::rust::String app_price_source() noexcept;

::rust::String app_price_currency() noexcept;

void app_set_price_selection(::rust::String source, ::rust::String currency) noexcept;

// Get the app language (a locale such as "fr"), empty until one is picked.
::rust::String app_language() noexcept;

// Set the app language.
void app_set_language(::rust::String locale) noexcept;

// The QR display tuning picked last, the middle level until one is.
::QrDisplayLevels app_qr_display_levels() noexcept;

// Remember the QR display tuning for every account.
void app_set_qr_display_levels(::QrDisplayLevels levels) noexcept;

// The camera contrast level picked last, the middle level until one is.
::std::uint8_t app_qr_camera_contrast_level() noexcept;

// Remember the camera contrast level for every account.
void app_set_qr_camera_contrast_level(::std::uint8_t level) noexcept;

// The `server` address last used to create an account on `network`, empty
// until one is.
::rust::String app_last_endpoint(::BackendServer server, ::Network network) noexcept;

// Remember the `server` address an account on `network` was created with.
void app_set_last_endpoint(::BackendServer server, ::Network network, ::rust::String url) noexcept;

// The app state's unlock key, before anything else reads the state.
::StateUnlockInfo state_unlock_info() noexcept;

// A state file found in clear, empty when there is none: the app stops.
::rust::String state_clear_file() noexcept;

// Forgets the key: the state cannot be read or written until the next unlock.
void state_lock() noexcept;

// A hardened index for a new device key, drawn from the system random source.
// A failed draw is an error: never a fixed index.
::StateIndexResult state_draw_index() noexcept;

// The path a device unlock key is fetched at, "m/138'/1936225652'/<index>'".
::rust::String state_device_path(::std::uint32_t index) noexcept;

// Creates the state, unlocked by the device whose xpub at the device path of
// `index` is `xpub`. Answers the error, empty on success.
::rust::String state_create_with_device(::std::uint32_t index, ::rust::String xpub) noexcept;

// Creates the state, unlocked by `password`. Blocks on Argon2id: off the GUI
// thread. Answers the error, empty on success.
::rust::String state_create_with_password(::rust::String password) noexcept;

// Unlocks with the device xpub. Answers the error, empty on success.
::rust::String state_unlock_with_device(::rust::String xpub) noexcept;

// Unlocks with `password`. Blocks on Argon2id: off the GUI thread. Answers the
// error, empty on success.
::rust::String state_unlock_with_password(::rust::String password) noexcept;

// Seals the plugin dropped at `path` (its folder, or its library with the
// sidecar beside it) into the state, replacing an earlier version of it.
::PluginInstallResult plugin_install(::rust::String path) noexcept;

::rust::String plugin_admission(::rust::Slice<::std::uint8_t const> bytes, ::rust::String expected_iid, ::std::uint8_t qt_major, ::std::uint8_t qt_minor, ::std::uint8_t arch_requirements) noexcept;

// Every installed plugin's sidecar, as JSON text.
::rust::Vec<::rust::String> plugin_bundles() noexcept;

::rust::Vec<::PluginMetadata> plugin_metadata() noexcept;

// The library and catalogs of the installed plugin `plugin_id`.
::PluginLibraryResult plugin_library(::rust::String plugin_id) noexcept;

// Removes the installed plugin `plugin_id`. Answers the error, empty on success.
::rust::String plugin_remove(::rust::String plugin_id) noexcept;

// Opens a storage session for the plugin the host authenticated as
// `plugin_id`, its library `plugin_hash`. Host only: the caller is never asked
// who it is. `external` false for a module the host ships, which needs no
// Saved Data grant. Answers 0 when refused.
::std::uint64_t storage_session_open(::rust::String plugin_id, ::rust::String plugin_hash, bool external) noexcept;

// Ends the session on unload: outstanding requests answer `revoked`. Never
// waits for a request.
void storage_session_revoke(::std::uint64_t session) noexcept;

// Answers the JSON storage request on `session` with a JSON response. Blocks
// on disk and on other sessions' requests: off the GUI thread.
::rust::String storage_request(::std::uint64_t session, ::rust::String request) noexcept;

// Cancels the request `id` on `session`, sent or not yet: it answers
// `cancelled`. A request already replacing its tree may complete.
void storage_request_cancel(::std::uint64_t session, ::rust::String id) noexcept;

// Forgets the cancellation of `id` on `session`, once its `storage_request`
// returned or will never be made.
void storage_request_forget(::std::uint64_t session, ::rust::String id) noexcept;

// The session's generation now, taken when a request is sent and given back
// to `storage_delivery_error` with its answer. 0 for a closed session.
::std::uint64_t storage_session_generation(::std::uint64_t session) noexcept;

// Why an answer computed at `generation` must not be delivered: "revoked",
// "locked", "denied" once the grant or the key changed, empty when it may.
// Reads memory only: safe on the GUI thread.
::rust::String storage_delivery_error(::std::uint64_t session, ::std::uint64_t generation) noexcept;

// The answers given to external plugin libraries, in the order they were given.
::rust::Vec<::PluginAccess> app_plugin_access() noexcept;

::PluginAccess app_plugin_answers(::rust::String plugin_id, ::rust::String hash) noexcept;

::PluginGrantedAccess app_plugin_grants(::rust::String hash) noexcept;

::rust::Vec<::PluginAccess> app_plugin_history(::rust::String plugin_id) noexcept;

// Stores the answers for `access.hash`, replacing earlier ones. An entry left
// with both grants unasked is dropped, so the host asks again.
void app_set_plugin_access(::PluginAccess access) noexcept;

// Writes the PSBT to `path` exactly as serialized. Refuses to overwrite.
::PsbtFileResult sd_save_psbt(::rust::Str path, ::rust::Str psbt_hex) noexcept;

::PsbtFileResult sd_load_psbt(::rust::Str path) noexcept;

// Writes the BIP138 backup of the musig() `descriptor` to `path`, replacing
// any file there. `store_common_paths` false is the privacy mode: the common
// derivation paths stay out of the clear part.
::DescriptorBackupResult save_descriptor_backup(::rust::Str descriptor, ::rust::Str path, bool store_common_paths) noexcept;

// Create a new config.
::AmountParseResult parse_btc_amount(::rust::Str text) noexcept;

::BackendErrorKind validation_error_kind(::PsbtValidation const &result) noexcept;

::BackendErrorKind notification_error_kind(::Notification const &notification) noexcept;

::FeeParseResult parse_fee_input(::rust::Str text, bool rate) noexcept;

::FeeParseResult normalize_fee_rate(double rate) noexcept;

::SendAmounts send_amounts(::rust::Slice<::std::uint64_t const> inputs, ::rust::Slice<::std::uint64_t const> recipients, ::std::uint64_t fee) noexcept;

::std::uint32_t policy_threshold(::std::uint32_t picked, ::std::uint32_t mandatory, ::std::uint32_t threshold) noexcept;

::std::uint64_t policy_duration_blocks(::std::uint32_t count, ::PolicyDurationUnit unit) noexcept;

::PolicyDraftResult resolve_policy_draft(::PolicyDraft draft, ::Network network, bool has_tip, ::std::uint32_t tip, ::std::int64_t now) noexcept;

::AccountCreationResult create_sp_account(::SpCreationRequest request) noexcept;

::AccountCreationResult create_musig_account(::MusigCreationRequest request) noexcept;

::rust::Box<::Config> new_config(::rust::String account_name, ::Network network, ::rust::String descriptor, ::rust::String blindbit_url, ::rust::String electrum_url, ::std::uint64_t dust_limit, ::rust::String plugin_id) noexcept;

// Load config from file.
::rust::Box<::Config> config_from_file(::rust::String account_name) noexcept;

// List all existing configs.
::rust::Vec<::rust::String> list_configs() noexcept;

::rust::Vec<::rust::String> list_module_configs(::rust::Str plugin_id) noexcept;

// Delete an account's data from disk.
::rust::String sp_account_delete_check(::rust::String name) noexcept;

::rust::String musig_account_delete_check(::rust::String name) noexcept;

::rust::String delete_sp_account(::rust::String name) noexcept;

::rust::String delete_musig_account(::rust::String name) noexcept;

// The segwit sub-account descriptor of a key, from its xpub at 84'/coin'/0'.
// Empty when the fingerprint or the xpub is invalid.
::rust::String segwit_descriptor_for(::rust::String fingerprint, ::rust::String xpub, ::Network network) noexcept;

// The taproot sub-account descriptor of a key, from its xpub at 86'/coin'/0'.
// Empty when the fingerprint or the xpub is invalid.
::rust::String taproot_descriptor_for(::rust::String fingerprint, ::rust::String xpub, ::Network network) noexcept;

// Create a new account from an account name (loads config from file).
// Check is_ok() before use; get_error() for failure reason.
::rust::Box<::Account> new_account(::rust::String account_name) noexcept;

::rust::Box<::Account> new_account_offline(::Config const &config) noexcept;

// Create the device service. Does not start polling.
::rust::Box<::HwiService> new_hwi_service(::Network network) noexcept;

// Network the device service runs on, persisted in the module's config file.
::Network hwi_config_network() noexcept;

void hwi_set_config_network(::Network network) noexcept;

// Whether a device's capability bits include silent payments.
bool supports_silent_payments(::std::uint32_t capabilities) noexcept;

::rust::Box<::KeysModule> new_keys_module(::Network network) noexcept;

// 12 or 24 words. Any other count is an error. Fast: only draws entropy and
// encodes a mnemonic, no password involved, so unlike the other hot
// functions below it does not block on Argon2id and is safe to call from
// the GUI thread.
::MnemonicResult generate_mnemonic(::std::uint32_t word_count) noexcept;

// Whether `word` is in the English BIP39 word list.
bool bip39_word_is_valid(::rust::Str word) noexcept;

// Whether `mnemonic` is a BIP39 mnemonic: known words, a supported length
// and a matching checksum. Any length BIP39 allows, so 15 words too.
bool mnemonic_is_valid(::rust::Str mnemonic) noexcept;

::rust::Box<::KeyFilePreview> key_file_preview(::rust::Str path, ::Network network) noexcept;

::rust::Box<::KeyFileCommit> commit_key_file(::KeyFilePreview const &preview, ::rust::Str alias) noexcept;

// Derives the fingerprint and seals `mnemonic` under `password` for
// `network`, touching no `KeysModule` state. Blocks on Argon2id: call from
// a QThread, never from the GUI thread.
::rust::Box<::SealedHotKey> hot_seal(::rust::Str mnemonic, ::rust::Str password, ::Network network) noexcept;

// Blocks on Argon2id while opening the envelope: call from a QThread,
// never from the GUI thread.
::MnemonicResult hot_reveal(::HotMaterial const &material, ::rust::Str password) noexcept;

// Signs `psbt_v2_hex` with `material`'s hot secret and returns the signed
// PSBT as v2 hex. Blocks on Argon2id while opening the envelope: call from
// a QThread, never from the GUI thread.
::SignResult hot_sign(::HotMaterial const &material, ::rust::Str password, ::rust::Str psbt_v2_hex) noexcept;

// The BIP392 sp() descriptor of `material`'s hot key, origin included, for
// account creation. Blocks on Argon2id while opening the envelope: call
// from a QThread, never from the GUI thread.
::HotSpDescriptorResult hot_sp_descriptor(::HotMaterial const &material, ::rust::Str password) noexcept;

// The xpub at `path` of `material`'s hot secret, for an account's segwit or
// taproot sub-account. Blocks on Argon2id while opening the envelope: call
// from a QThread, never from the GUI thread.
::HotXpubResult hot_xpub(::HotMaterial const &material, ::rust::Str password, ::rust::Str path) noexcept;

// Converts a v2 PSBT into its v0 base64 payload for a USB signing device.
// Cheap; safe to call from the GUI thread. Refuses a PSBT with a pending SP
// output: a signing device only ever signs its own bip32 inputs, and the
// signing plan already blocks every other candidate while one is pending. In
// MuSig2 round 1 every nonce and partial signature is dropped, as the QR route
// does, because a device takes any nonce in the PSBT as round 2;
// `usb_merge_signed` merges into the unstripped original, so the nonces
// already gathered stay. With `open_musig_session` (a Coldcard) and a wallet
// `descriptor` whose internal key is a musig(), an input listing no MuSig2
// participants gets the internal key's: the Coldcard needs a MuSig2 session
// for such a wallet even on a single-key leaf. The nonce it may add for that
// key path is dropped by `usb_merge_signed`.
::PsbtPayloadResult usb_sign_payload(::rust::Str psbt_v2_hex, ::rust::Str descriptor, bool open_musig_session) noexcept;

// Merges a USB device's v0 signed answer back into the original v2 PSBT,
// keeping its silent-payment input tweaks and ECDH shares. Only signature
// fields are copied. Cheap; safe to call from the GUI thread.
::SignResult usb_merge_signed(::rust::Str original_v2_hex, ::rust::Str signed_v0_base64) noexcept;

::MusigSessionCreateResult hot_create_sessions(::HotMaterial const &material, ::rust::Str password, ::rust::Str policy, ::std::uint32_t quantity) noexcept;

::HotCachedNonceResult hot_cached_nonces(::HotMaterial const &material, ::rust::Str psbt_v2_hex) noexcept;

::KeyOpResult musig_session_refresh(::rust::Str fingerprint) noexcept;

::std::uint32_t musig_session_count(::rust::Str fingerprint, ::rust::Str policy, ::KeyRouteKind provider) noexcept;

::std::uint32_t musig_session_limit() noexcept;

::SigningPlan musig_session_plan(::Network network, ::rust::Str psbt_v2_hex, ::rust::Vec<::LiveDevice> devices) noexcept;

::SignResult musig_session_restart(::Network network, ::rust::Str policy, ::rust::Str original, ::rust::Str prepared) noexcept;

::rust::Box<::LedgerNonceFetch> ledger_nonce_fetch(::rust::Str fingerprint, ::rust::Str policy, ::std::uint32_t quantity) noexcept;

::rust::Box<::LedgerNonceFetch> ledger_nonce_fetch_for_signing(::rust::Str fingerprint, ::rust::Str policy, ::rust::Str psbt_v2_hex) noexcept;

::KeyOpResult ledger_nonce_refresh(::rust::Str fingerprint) noexcept;

::SigningPlan ledger_nonce_plan(::Network network, ::rust::Str psbt_v2_hex, ::rust::Vec<::LiveDevice> devices) noexcept;

::SignResult ledger_nonce_invalidate(::rust::Str fingerprint, ::rust::Str policy, ::rust::Str psbt_v2_hex) noexcept;

::SignResult ledger_nonce_restart(::Network network, ::rust::Str policy, ::rust::Str original, ::rust::Str prepared) noexcept;

::std::uint32_t ledger_nonce_count(::rust::Str fingerprint, ::rust::Str policy) noexcept;

::std::uint32_t ledger_nonce_limit() noexcept;

::SignResult ledger_nonce_merge(::rust::Str fingerprint, ::rust::Str policy, ::rust::Str original, ::rust::Str signed_psbt) noexcept;

::LedgerNonceSignResult ledger_nonce_sign(::rust::Str fingerprint, ::rust::Str policy, ::rust::Str psbt_v2_hex) noexcept;

// Merges a signer's v2 answer, made on an earlier copy of the PSBT, into
// `current_v2_hex`: its signatures and MuSig2 nonces / partial signatures join
// the ones gathered since, so several signers can work at once. An answer
// adding nothing new leaves `current_v2_hex` as it is. Cheap; GUI thread.
::SignResult merge_signed_psbt(::rust::Str current_v2_hex, ::rust::Str signed_v2_hex) noexcept;

::CameraListResult camera_list() noexcept;

// Spawns the capture thread for camera `index`; the device opens inside it,
// and a failure there shows up later through `failure()`. Call from a
// worker thread, never the GUI thread.
::rust::Box<::CameraHandle> camera_open(::std::uint32_t index) noexcept;

// Create a new SyncEstimator with default EMA alpha.
::rust::Box<::SyncEstimator> new_sync_estimator() noexcept;

// Parses BIP-390 text and returns its canonical form.
::MusigDescriptorResult validate_musig_descriptor(::rust::String descriptor) noexcept;

// Every cosigner fingerprint of a musig() descriptor, empty on error.
::rust::Vec<::rust::String> musig_descriptor_fingerprints(::rust::String descriptor) noexcept;

// The receive address a new account of a musig() descriptor hands out first.
::MusigReceiveAddress musig_first_receive_address(::rust::String descriptor, ::Network network) noexcept;

// Loads the account `account_name` (config plugin id "musig").
// Check is_ok() before use.
::rust::Box<::MusigAccount> new_musig_account(::rust::String account_name) noexcept;

::rust::Box<::MusigAccount> new_musig_account_offline(::Config const &config) noexcept;

#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__
