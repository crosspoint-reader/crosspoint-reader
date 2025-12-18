#pragma once

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/bookerly_2b.h",
 *    "./lib/EpdFont/builtinFonts/bookerly_bold_2b.h",
 *    "./lib/EpdFont/builtinFonts/bookerly_bold_italic_2b.h",
 *    "./lib/EpdFont/builtinFonts/bookerly_italic_2b.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define READER_FONT_ID 1818981670

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/ubuntu_10.h",
 *    "./lib/EpdFont/builtinFonts/ubuntu_bold_10.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define UI_FONT_ID (-1619831379)

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/pixelarial14.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define SMALL_FONT_ID (-139796914)
