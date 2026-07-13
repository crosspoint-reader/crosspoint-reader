/* bidi_pairs.t — unified mirror + bracket table for CrossPoint.
 *
 * Replaces both mirroring.t and brackets.t.  canonical.t is dropped
 * (fullwidth brackets are not used in Hebrew epub content).
 *
 * Each entry: {from, to, bracket_type}
 *   bracket_type == BRACKo : `from` is an opening bracket
 *   bracket_type == BRACKc : `from` is a closing bracket; `to` = opener
 *   bracket_type == BRACKx : not a bracket pair — mirrored by rule L4 only
 *
 * mirror(c)  → always returns `to` for any entry where c==from
 * bracket(c) → returns 0 for BRACKx; c for BRACKo; `to` (opener) for BRACKc
 *
 * Sorted ascending by `from` (binary search).
 */

/* ASCII brackets — both bracket pairs AND L4 mirrors */
{0x0028, 0x0029, BRACKo},   /* ( */
{0x0029, 0x0028, BRACKc},   /* ) */
{0x003C, 0x003E, BRACKo},   /* < */
{0x003E, 0x003C, BRACKc},   /* > */
{0x005B, 0x005D, BRACKo},   /* [ */
{0x005D, 0x005B, BRACKc},   /* ] */
{0x007B, 0x007D, BRACKo},   /* { */
{0x007D, 0x007B, BRACKc},   /* } */

/* Angle quotation marks — L4 mirror only */
{0x00AB, 0x00BB, BRACKx},   /* « → » */
{0x00BB, 0x00AB, BRACKx},   /* » → « */

/* Curly quotes — L4 mirror only */
{0x2018, 0x2019, BRACKx},   /* ' → ' */
{0x2019, 0x2018, BRACKx},   /* ' → ' */
{0x201C, 0x201D, BRACKx},   /* " → " */
{0x201D, 0x201C, BRACKx},   /* " → " */

/* Single angle quotes */
{0x2039, 0x203A, BRACKo},   /* ‹ → › */
{0x203A, 0x2039, BRACKc},   /* › → ‹ */
