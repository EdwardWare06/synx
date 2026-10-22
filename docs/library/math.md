# std.math

```synx
import std.math;
import std.math.{Vec3, sqrt, PI};
```

This module holds scalar functions on `double` values, two integer functions
on `ulong` values, and the three-dimensional vector type `Vec3`. All
functions are written in Synx. Floating-point operations do not trap on
infinity or NaN. The results are floating-point approximations.

## Types

### Vec3

```synx
struct Vec3 {
    double x;
    double y;
    double z;
}
```

A vector with three `double` components. The fields are public. Create a
value with a struct literal, such as `Vec3 { x: 1.0, y: 0.0, z: 0.0 }`. The
methods do not change the vector. They return new values.

### Vec3.dot

```synx
double dot(&const self, &const Vec3 other)
```

Returns the dot product of this vector and *other*.

### Vec3.length

```synx
double length(&const self)
```

Returns the length of the vector. The function divides each component by
the largest absolute component before it squares them. This reduces
overflow and underflow in the middle of the calculation. The result can
still be infinite. A zero vector gives
`0.0`. If a component is infinite, the result is infinity. If a component
is NaN, the result is NaN.

### Vec3.add

```synx
Vec3 add(&const self, &const Vec3 other)
```

Returns the sum of this vector and *other*, component by component.

### Vec3.sub

```synx
Vec3 sub(&const self, &const Vec3 other)
```

Returns this vector minus *other*, component by component.

### Vec3.scale

```synx
Vec3 scale(&const self, double factor)
```

Returns this vector with each component multiplied by *factor*.

### Vec3.cross

```synx
Vec3 cross(&const self, &const Vec3 other)
```

Returns the cross product of this vector and *other*.

### Vec3.distance

```synx
double distance(&const self, &const Vec3 other)
```

Returns the length of this vector minus *other*.

### Vec3.normalized

```synx
Vec3? normalized(&const self)
```

Returns a vector with the same direction and length 1.0. The function
divides by the largest absolute component first, as [length](#vec3length)
does. Returns `None` for a zero vector, and for a vector with an infinite
or NaN component.

## Functions

### abs

```synx
double abs(double value)
```

Returns the absolute value of *value*. Zero and negative zero give `0.0`.
NaN gives NaN.

### is_nan

```synx
bool is_nan(double value)
```

Returns `true` if *value* is NaN.

### is_finite

```synx
bool is_finite(double value)
```

Returns `true` if *value* is neither infinite nor NaN.

### min

```synx
double min(double a, double b)
```

Returns the smaller of *a* and *b*. If either argument is NaN, the result
is NaN. If the arguments compare equal, the result is *b*.

### max

```synx
double max(double a, double b)
```

Returns the larger of *a* and *b*. If either argument is NaN, the result
is NaN. If the arguments compare equal, the result is *b*.

### clamp

```synx
double clamp(double value, double low, double high)
```

Returns *low* if *value* is less than *low*, *high* if *value* is greater
than *high*, and *value* in other cases. A NaN *value* is returned
unchanged.

Panics with `clamp requires ordered bounds` if *low* is greater than *high*
or if either bound is NaN.

### trunc

```synx
double trunc(double value)
```

Returns *value* with its fractional part removed, rounded toward zero.
Infinity, NaN, and values with an absolute value of 2^52 or more are
returned unchanged. A negative value between -1.0 and 0.0 gives `0.0`. The
sign of zero is not kept.

### floor

```synx
double floor(double value)
```

Returns the largest whole number that is not greater than *value*.

### ceil

```synx
double ceil(double value)
```

Returns the smallest whole number that is not less than *value*.

### round

```synx
double round(double value)
```

Returns the nearest whole number. A value halfway between two whole numbers
is rounded away from zero, so `round(2.5)` is `3.0` and `round(-2.5)` is
`-3.0`.

### lerp

```synx
double lerp(double start, double end, double amount)
```

Returns the linear interpolation `(1.0 - amount) * start + amount * end`.
An *amount* of `0.0` returns *start* exactly. An *amount* of `1.0` returns
*end* exactly. *amount* is not limited to the range 0.0 to 1.0.

### radians

```synx
double radians(double degrees)
```

Converts an angle in degrees to radians.

### degrees

```synx
double degrees(double radians)
```

Converts an angle in radians to degrees.

### powi

```synx
double powi(double value, int exponent)
```

Returns *value* raised to the integer power *exponent*. The function uses
repeated squaring. A negative *exponent* uses the reciprocal of *value*.
An *exponent* of zero gives `1.0`. All `int` exponents are accepted,
including the smallest one.

### approx_eq

```synx
bool approx_eq(double a, double b, double relative, double absolute)
```

Returns `true` if *a* and *b* are close. The allowed difference is the
larger of *absolute* and *relative* times the larger of `abs(a)` and
`abs(b)`.

Two equal values, also two equal infinities, give `true`. Any other pair
with an infinite or NaN value gives `false`. A negative or NaN tolerance
gives `false`.

### gcd

```synx
ulong gcd(ulong a, ulong b)
```

Returns the greatest common divisor of *a* and *b*. `gcd(a, 0)` is *a*.
`gcd(0, 0)` is `0`.

### lcm

```synx
ulong? lcm(ulong a, ulong b)
```

Returns the least common multiple of *a* and *b*. Returns `Some(0)` if
either argument is zero. Returns `None` if the result is larger than the
largest `ulong`. The overflow check is the same in debug and release
builds.

### sqrt

```synx
double sqrt(double value)
```

Returns the square root of *value*. The function uses Newton iteration.
Zero, negative zero, positive infinity, and NaN are returned unchanged. The
result can differ from the correctly rounded square root in the last bit.
For example, `sqrt(2.0)` returns `1.414213562373095`.

Panics with `sqrt requires a nonnegative value` if *value* is less than
zero.

## Constants

### PI

```synx
const PI = 3.141592653589793;
```

The ratio of the circumference of a circle to its diameter.

### TAU

```synx
const TAU = 6.283185307179586;
```

Two times `PI`.

## Example

```synx
import std.math;
import std.math.Vec3;

main() {
    print("round(2.5) = {math.round(2.5)}, round(-2.5) = {math.round(-2.5)}");
    print("floor(-2.5) = {math.floor(-2.5)}, ceil(-2.5) = {math.ceil(-2.5)}");
    print("clamp(1.5, 0.0, 1.0) = {math.clamp(1.5, 0.0, 1.0)}");
    print("powi(2.0, -3) = {math.powi(2.0, -3)}");
    print("0.1 + 0.2 == 0.3: {0.1 + 0.2 == 0.3}");
    print("approx_eq: {math.approx_eq(0.1 + 0.2, 0.3, 1e-12, 0.0)}");
    print("gcd(84, 36) = {math.gcd(84, 36)}");
    match math.lcm(18446744073709551615, 2) {
        Some(value) => print("lcm = {value}"),
        None => print("lcm overflows ulong"),
    }

    var a = Vec3 { x: 3.0, y: 0.0, z: 4.0 };
    var up = Vec3 { x: 0.0, y: 1.0, z: 0.0 };
    print("length {a.length()}, dot {a.dot(&up)}");
    var c = a.cross(&up);
    print("cross ({c.x}, {c.y}, {c.z})");
    match a.normalized() {
        Some(n) => print("unit ({n.x}, {n.y}, {n.z})"),
        None => print("no direction"),
    }
}
```

Output:

```text
round(2.5) = 3.0, round(-2.5) = -3.0
floor(-2.5) = -3.0, ceil(-2.5) = -2.0
clamp(1.5, 0.0, 1.0) = 1.0
powi(2.0, -3) = 0.125
0.1 + 0.2 == 0.3: false
approx_eq: true
gcd(84, 36) = 12
lcm overflows ulong
length 5.0, dot 0.0
cross (-4.0, 0.0, 3.0)
unit (0.6, 0.0, 0.8)
```
