// Indirect calls: virtual dispatch (single, multiple, virtual inheritance, dynamic_cast/RTTI),
// function-pointer tables, pointer-to-member-function, std::function-style callbacks, qsort comparator.
#include "ps5test.h"
#include <stdlib.h>
#include <string.h>

struct Shape {
  virtual ~Shape() {}
  virtual uint64_t area(uint64_t k) const = 0;
  virtual const char *name() const { return "shape"; }
};
struct Sq : Shape {
  uint64_t s; explicit Sq(uint64_t s) : s(s) {}
  uint64_t area(uint64_t k) const override { return s * s + k; }
  const char *name() const override { return "sq"; }
};
struct Rect : Sq {
  uint64_t w; Rect(uint64_t s, uint64_t w) : Sq(s), w(w) {}
  uint64_t area(uint64_t k) const override { return s * w + 2 * k; }
};
struct Tri : Shape {
  uint64_t b, h; Tri(uint64_t b, uint64_t h) : b(b), h(h) {}
  uint64_t area(uint64_t k) const override { return b * h / 2 + 3 * k; }
};
struct Named { virtual ~Named() {} virtual int id() const { return 100; } };
struct Both : Tri, Named { Both() : Tri(6, 7) {} int id() const override { return 200; } uint64_t area(uint64_t k) const override { return 999 + k; } };
struct VB { int v = 5; virtual ~VB() {} virtual int get() const { return v; } };
struct VL : virtual VB { int get() const override { return v + 1; } };
struct VR : virtual VB { int get() const override { return v + 2; } };
struct VD : VL, VR { int get() const override { return VL::get() * 10 + VR::get(); } };

typedef uint64_t (*Op)(uint64_t, uint64_t);
static uint64_t op_add(uint64_t a, uint64_t b) { return a + b; }
static uint64_t op_mul(uint64_t a, uint64_t b) { return a * b; }
static uint64_t op_xor(uint64_t a, uint64_t b) { return a ^ (b << 3); }
static uint64_t op_sub(uint64_t a, uint64_t b) { return a - b; }
static Op g_ops[] = {op_add, op_mul, op_xor, op_sub};

struct Calc { uint64_t base; uint64_t plus(uint64_t x) { return base + x; } uint64_t times(uint64_t x) { return base * x; } };
static int cmp_desc(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x < y) - (x > y); }
static T_NOINLINE uint64_t apply_cb(uint64_t (*cb)(void *, uint64_t), void *ctx, uint64_t v) { return cb(ctx, cb(ctx, v)); }
static uint64_t cb_calc(void *c, uint64_t v) { return ((Calc *)c)->plus(v) * 3; }

int main() {
  Shape *objs[] = {new Sq(5), new Rect(3, 4), new Tri(8, 3), new Both()};
  uint64_t h = T_FNV0;
  for (int rep = 0; rep < 3; rep++) for (Shape *s : objs) { uint64_t a = s->area(rep); h = t_fnv(h, &a, 8); h = t_fnv(h, s->name(), strlen(s->name())); }
  t_hex("virtual_fnv", h);
  int rtti = 0;
  for (Shape *s : objs) {
    if (dynamic_cast<Sq *>(s)) rtti += 1;
    if (dynamic_cast<Rect *>(s)) rtti += 10;
    if (Named *n = dynamic_cast<Named *>(s)) rtti += 100 + n->id();
    if (dynamic_cast<Tri *>(s)) rtti += 1000;
  }
  t_dec("rtti", rtti);
  VD vd; VB *vb = &vd;
  t_dec("vbase_get", vb->get());
  t_dec("vbase_cast", dynamic_cast<VR *>(vb)->get());
  uint64_t acc = 7;
  for (int i = 0; i < 64; i++) acc = g_ops[(acc + i) & 3](acc, i + 1);
  t_hex("fnptr_table", acc);
  Calc c{11};
  uint64_t (Calc::*pm[2])(uint64_t) = {&Calc::plus, &Calc::times};
  uint64_t m = 0; for (int i = 0; i < 6; i++) m += (c.*pm[i & 1])(i);
  t_dec("memfn_ptr", (long long)m);
  t_hex("callback", apply_cb(cb_calc, &c, 5));
  int arr[16]; uint64_t s = 3; for (int i = 0; i < 16; i++) arr[i] = (int)(t_lcg(&s) % 1000);
  qsort(arr, 16, sizeof(int), cmp_desc);
  h = t_fnv(T_FNV0, arr, sizeof arr); t_hex("qsort_fnv", h);
  volatile Op vop = op_mul; t_hex("volatile_fp", vop(6, 7));
  for (Shape *o : objs) delete o;
  return 0;
}
