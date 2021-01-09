An attempt at a waveshaper effect.

For order 3, for instance, the equation is:

	y[2] = (index*y[2])**3 * scale(3) + (index*y[1])**2 * scale(2) + (index*y[0]) * scale(1)

Scale is calculated like this:

    scale(x) = scaleoffset + scalepowbase**(x + scalepowexpoffset)

`scalepowbase` and `scaleoffset` can be set to -1 to pass only odd-numbered harmonics.
