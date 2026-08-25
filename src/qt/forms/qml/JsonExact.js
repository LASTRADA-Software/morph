.pragma library

// JSON parsing that does not silently round an integer a double cannot hold.
//
// Every value the renderer receives from an app -- query results, choice option
// rows -- arrives as JSON text and used to go through plain `JSON.parse`.
// JavaScript numbers are IEEE-754 doubles, so an integer above 2^53 does not
// survive: it is rounded on the way in, and re-serialising the rounded number
// emits a *different* id than the app sent. Because doubles round to even in
// that range, neighbouring ids collapse onto the same value, so two distinct
// rows can become indistinguishable -- deleting one deletes the other
// (morph#190, morph#191).
//
// The fix keeps the digits. parse() finds integer literals a double cannot
// represent exactly and hands them back as exact-int wrappers carrying the
// original text; literal() re-emits those digits verbatim. Values a double
// *does* hold exactly -- the overwhelmingly common case -- stay ordinary
// numbers, so nothing changes for them.
//
// Why not a hand-written JSON parser: the sentinel round trip below reuses the
// engine's own parser, which is the part that must stay exactly right. Only
// number-literal recognition is ours.

// A NUL followed by "E" cannot begin a string in any payload worth accepting:
// JSON forbids an unescaped NUL, so producing one takes a deliberate \u0000
// escape. parse() refuses the transform outright when the payload contains one
// anyway, rather than risk mistaking app data for a sentinel.
// The marker as it appears *after* parsing: a real NUL, then "E".
var SENTINEL = "\u0000E"

// The same marker as it must be written *into* JSON text. JSON forbids a raw
// control character inside a string, so the escape has to go in literally --
// writing the NUL itself produces text the parser rejects.
var SENTINEL_ESCAPED = "\\u0000E"

function makeExact(digits) {
    // toString() means String(v), string concatenation and QML text bindings
    // all render the exact digits with no call-site changes.
    return { __exactInt: digits, toString: function () { return digits } }
}

// True for a value parse() produced from an integer literal too large for a
// double to hold exactly.
function isExact(value) {
    return value !== null && typeof value === "object" && typeof value.__exactInt === "string"
}

// The exact decimal digits of `value`, for display or for use as a key.
function text(value) {
    return isExact(value) ? value.__exactInt : String(value)
}

// JSON text for `value`. An exact int emits its digits unquoted, so an action
// body carries the id the app actually sent rather than a rounded double.
// Everything else is ordinary JSON.stringify.
function literal(value) {
    return isExact(value) ? value.__exactInt : JSON.stringify(value)
}

// True when `token` (an integer literal, no fraction or exponent) survives a
// double round trip unchanged.
function fitsDouble(token) {
    return String(Number(token)) === token
}

// Rewrites every inexact integer literal in `jsonText` to a sentinel string, so
// the engine's parser carries the digits through untouched.
function protectLiterals(jsonText) {
    var out = ""
    var i = 0
    var inString = false
    while (i < jsonText.length) {
        var ch = jsonText.charAt(i)
        if (inString) {
            out += ch
            // A backslash escapes the next character, including a quote, so skip
            // it wholesale rather than let it close the string.
            if (ch === "\\" && i + 1 < jsonText.length) {
                out += jsonText.charAt(i + 1)
                i += 2
                continue
            }
            if (ch === "\"")
                inString = false
            i += 1
            continue
        }
        if (ch === "\"") {
            inString = true
            out += ch
            i += 1
            continue
        }
        if (ch === "-" || (ch >= "0" && ch <= "9")) {
            var start = i
            if (jsonText.charAt(i) === "-")
                i += 1
            while (i < jsonText.length && jsonText.charAt(i) >= "0" && jsonText.charAt(i) <= "9")
                i += 1
            var isInteger = true
            // A fraction or exponent makes this an already-approximate number,
            // not an id; leave those alone.
            var next = jsonText.charAt(i)
            if (next === "." || next === "e" || next === "E") {
                isInteger = false
                while (i < jsonText.length && "0123456789.eE+-".indexOf(jsonText.charAt(i)) !== -1)
                    i += 1
            }
            var token = jsonText.substring(start, i)
            out += (isInteger && !fitsDouble(token)) ? ("\"" + SENTINEL_ESCAPED + token + "\"") : token
            continue
        }
        out += ch
        i += 1
    }
    return out
}

// Replaces sentinel strings with exact-int wrappers, depth-first.
function reviveSentinels(value) {
    if (typeof value === "string")
        return (value.indexOf(SENTINEL) === 0) ? makeExact(value.substring(SENTINEL.length)) : value
    if (Array.isArray(value)) {
        for (var i = 0; i < value.length; ++i)
            value[i] = reviveSentinels(value[i])
        return value
    }
    if (value !== null && typeof value === "object") {
        for (var key in value)
            value[key] = reviveSentinels(value[key])
        return value
    }
    return value
}

// JSON.parse, except integer literals a double cannot hold exactly come back as
// exact-int wrappers (see isExact()/literal()). Throws what JSON.parse throws,
// so callers keep their existing try/catch.
function parse(jsonText) {
    // If the payload already carries a NUL, the sentinel is not unambiguous.
    // Parse plainly rather than risk reading app data as a marker: the rounding
    // this module exists to prevent is a bug, but misreading a string as an id
    // would be a worse one.
    if (jsonText.indexOf("\u0000") !== -1)
        return JSON.parse(jsonText)
    return reviveSentinels(JSON.parse(protectLiterals(jsonText)))
}
