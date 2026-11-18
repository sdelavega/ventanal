# miniz tinfl

Ventanal builds the portable C inflater from
[miniz 3.1.2](https://github.com/richgel999/miniz/tree/77d0dce8627735138c51770d1799a1ef48f2117d)
(commit `77d0dce8627735138c51770d1799a1ef48f2117d`) for the ESP32-S3 and
the AddressSanitizer host tests. The shared configuration selects 32-bit
bit buffers and portable byte loads and prefixes exported inflater symbols
to avoid binding to the Espressif mask ROM.

The vendored files retain the upstream [MIT (Expat) license](LICENSE), which
is [compatible with GPL-3.0-or-later](https://www.gnu.org/licenses/license-list.html#Expat).
The only upstream source change is the include in `miniz_tinfl.c` to select
the local tinfl-only configuration. The license text uses LF line endings.
