package svgdoc

// The documents the tests build: everything the renderer asks a document
// for, and everything the readers treat specially.

import (
	"encoding/xml"
	"fmt"
	"os"
	"sort"
	"strings"
	"testing"
	"unicode/utf16"

	"github.com/shibukawa/bdf/image/svg"
)

const head = `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="80" viewBox="0 0 100 80">`

func utf16Of(s string, bigEndian, mark bool) []byte {
	var b []byte
	put := func(u uint16) {
		if bigEndian {
			b = append(b, byte(u>>8), byte(u))
		} else {
			b = append(b, byte(u), byte(u>>8))
		}
	}
	if mark {
		put(0xfeff)
	}
	for _, u := range utf16.Encode([]rune(s)) {
		put(u)
	}
	return b
}

// builtDocs is the documents the tests make themselves, by name.
func builtDocs() map[string][]byte {
	docs := map[string][]byte{}
	add := func(name, doc string) {
		if _, dup := docs[name]; dup {
			panic("two documents named " + name)
		}
		docs[name] = []byte(doc)
	}

	// what the renderer in raster/imagebdf/svg.go asks for
	add("renderer", head+`
<title>t</title><desc>d</desc>
<defs>
  <linearGradient id="lg" x1="0" y1="0" x2="1" y2="0" gradientUnits="objectBoundingBox" gradientTransform="rotate(45)" spreadMethod="pad">
    <stop offset="0" stop-color="red"/><stop offset="50%" style="stop-color: #00f; stop-opacity: .5"/><stop offset="1" stop-color="rgb(0,255,0)" stop-opacity="0.25"/>
  </linearGradient>
  <radialGradient id="rg" cx="50%" cy="50%" r="50%" fx="30%" fy="30%" xlink:href="#lg"/>
  <pattern id="pat" x="0" y="0" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="scale(2)" viewBox="0 0 10 10"><rect width="5" height="5"/></pattern>
  <clipPath id="clip" clipPathUnits="objectBoundingBox" transform="translate(1 2)"><circle cx=".5" cy=".5" r=".5"/><use xlink:href="#sq" x="1" y="2"/></clipPath>
  <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="100" height="80"><rect width="100" height="80" fill="white"/></mask>
  <symbol id="sym" viewBox="0 0 10 10" preserveAspectRatio="xMidYMid slice"><path d="M0 0L10 10z"/></symbol>
  <rect id="sq" width="10" height="10"/>
</defs>
<g id="layer" transform="translate(10,20) scale(2) rotate(30 5 5) skewX(10) skewY(-10) matrix(1 0 0 1 3 4)" opacity="0.5" display="inline" visibility="visible"
   fill="currentColor" color="hsl(120, 50%, 50%)" stroke="url(#lg)" stroke-width="2px" stroke-linecap="round" stroke-linejoin="bevel" stroke-miterlimit="4"
   stroke-dasharray="4 2, 1" stroke-dashoffset="1" clip-path="url(#clip)" mask="url(#m)" overflow="hidden">
  <rect x="1" y="2" width="30" height="20" rx="3" ry="150%" fill="url(#pat)"/>
  <circle cx="50" cy="40" r="1.5em"/><ellipse cx="50" cy="40" rx="20" ry="10"/>
  <line x1="0" y1="0" x2="100%" y2="80"/><polyline points="1,2 3-4.5.5 6e1,7"/><polygon points="0,0 10,0 5,8"/>
  <path d="M10 10 h 80 v 60 h -80 Z m 5,5 c 1 2 3 4 5 6 a 5 5 0 0 1 10 10"/>
  <use href="#sym" xlink:href="#sq" x="5" y="5" width="20" height="20"/>
  <image x="0" y="0" width="10" height="10" preserveAspectRatio="none" xlink:href="data:image/png;base64,iVBORw0KGgo="/>
  <svg x="1" y="2" width="50%" height="50%" viewBox="0 0 4 4" preserveAspectRatio="xMinYMax meet"><rect width="4" height="4"/></svg>
  <switch><g requiredExtensions="http://example.com/x"><rect/></g><g systemLanguage="ja"><rect/></g><g><rect/></g></switch>
  <a xlink:href="http://example.com/">link <tspan>text</tspan></a>
</g>
<text x="10 20 30" y="40" dx="1 2" dy="3" font-family="'Noto Sans', sans-serif" font-size="12pt" font-style="italic" font-weight="bold"
      letter-spacing="1" text-anchor="middle" fill="#ABCDEF80">Hello, <tspan x="5" dy="1.2em" font-weight="bolder">world</tspan>
  <textPath href="#sq">on a path</textPath>&#x41;&amp;&nbsp;!</text>
</svg>`)

	// style sheets: the selectors the CSS reader takes and those it leaves out
	add("css-selectors", head+`<style type="text/css"><![CDATA[
rect { fill: red; stroke: blue }
.a { fill: green }
#one { fill: blue }
g rect.a { stroke-width: 2 }
g > rect#one.a.b { opacity: .5 }
* { visibility: visible }
*.b { display: inline }
rect, circle , .c ,#two { stroke: black }
svg g g rect { color: red }
.a.b { font-size: 12px }
#one.a { font-size: 14px }
rect[fill] { fill: none }
rect:hover { fill: pink }
a:first-child, circle { mask: none }
g>circle{fill:#123}
 > g { x: 1 }
.nope { x: 2 }
]]></style>
<g class="a b"><rect id="one" class="a  b a"/><rect class="b"/><g><rect class="a" id="two"/><circle class="c"/></g></g><circle/><rect id="one"/></svg>`)
	add("css-at-rules", head+`<style>@import url(x.css); @media print { rect { fill: red } .a { x: y } } @font-face { font-family: x; src: url(y) }
rect { fill: blue } @keyframes k { from { a: b } to { a: c } } circle { fill: green } @media { unterminated { </style><rect/><circle/></svg>`)
	add("css-comments", head+`<style>/* one */ rect /* two */ { fill /* three */ : red; /* four */ } /* five</style><style>*/ circle { fill: blue } /**/ g { x: 1 } /*/ a { b: c } /* */ e { f: g }</style><rect/><circle/><g/><a/><e/></svg>`)
	add("css-declarations", head+`<style>rect { FILL: Red !important; fill : blue ; ; x ; : y ; a:b:c ; stroke: url(http://x/y;z) ; É: 1 ; ÉCOLE: 2; İX: 3; K: 4; `+"\u00a0pad\u00a0:\u00a0v\u00a0 ; \xff\xfe: \xff ;"+`
 !important: 1; q: !important; r: x !IMPORTANT; s: a!important }
rect { fill: green; stroke-width: 3 }
circle { nested { a: b } c: d }
ellipse {} line { }
</style><rect style="FILL : Red !important ;fill:blue;;x;:y;a:b:c; É : 1; İX: 3; STROKE:`+"\u3000 a b \u3000"+`" fill=" none " stroke="  "/><circle/><ellipse/><line/></svg>`)
	add("css-specificity", head+`<style>.a { p: class } rect { p: tag; q: tag } #i { p: id; r: id } .a { q: class2 } rect.a { s: both } * { t: star; p: star } .b.a { u: two }</style>
<rect class="a b" id="i"/><rect class="a"/><rect/><circle class="a" id="i"/></svg>`)
	add("css-descendants", head+`<style>g rect { a: 1 } g g rect { b: 2 } svg > g > rect { c: 3 } .x .y .z { d: 4 } .x .x { e: 5 } #r g { f: 6 } text tspan { g: 7 } g a { h: 8 }</style>
<g class="x" id="r"><rect class="z"/><g class="y x"><rect class="z"/><g><rect class="z y"/></g></g></g><text><tspan>t</tspan>u</text></svg>`)
	add("css-many-sheets", head+`<style>rect{a:1}</style><g><style>rect{a:2;b:1}</style></g><defs><style>rect{c:3}<!-- comment --></style></defs><rect/><style>rect{a:4}</style></svg><style>rect{z:9}</style>`)
	add("css-entities", head+`<style>g &gt; rect { a: 1 } rect &#123; b: 2 &#125; .a&amp;b { c: 3 } circle { d: "&lt;" }</style><g><rect class="a&amp;b"/></g><circle/></svg>`)
	add("css-odd-selectors", head+`<style>. { a: 1 } # { b: 2 } .. { c: 3 } .a. { d: 4 } rect# { e: 5 } #a#b { f: 6 } rect.a#b.c { g: 7 } , rect, { h: 8 } rect,, circle { i: 9 }
*{ j: 1 } ** { k: 1 } *rect { l: 1 } rect* { m: 1 } `+"g\u00a0rect\u2003.a { n: 1 } r\xffct { o: 1 } .\xc3 { p: 1 }"+` RECT { q: 1 } Rect { r: 1 } .A { s: 1 }</style>
<g><rect class="a" id="b"/><rect class="a c" id="b"/><circle/></g><RECT/><rect class="A"/></svg>`)
	add("css-class-attr", head+`<style>.a{p:1}.b{q:2}.c{r:3}</style><rect class="a"/><rect class=" a  b "/><rect class="a`+"\u00a0b\u2003c\tA"+`"/><rect class=""/><rect class="  "/><rect class="b b b a a"/><rect class="`+"\xffa \xc3"+`"/></svg>`)
	add("css-combinators", head+`<style>> { a: b }</style><rect/></svg>`)
	add("css-combinators-2", head+`<style>rect { a: b } g, > > , circle { c: d }</style><rect/></svg>`)
	add("css-combinators-space", head+"<style>rect{a:b} \u00a0>\u2003> {c:d}</style><rect/></svg>")
	add("css-combinators-not-svg", `<notsvg><style>> { a: b }</style></notsvg>`)
	add("css-combinators-beside", head+`<style>> g { a: b } g > { c: d } >g>rect> { e: f }</style><g><rect/></g></svg>`)
	add("css-unterminated", head+`<style>rect { fill: red } circle { fill: blue</style><rect/><circle/></svg>`)
	add("css-braces", head+`<style>rect { a: { b } } } circle { c: d } } { e: f } ellipse { g: h }</style><rect/><circle/><ellipse/></svg>`)
	add("css-empty", head+`<style></style><style> </style><style><![CDATA[]]></style><rect/></svg>`)
	add("css-style-in-text", head+`<text><style>tspan{a:1}</style><tspan>x</tspan></text><style>text<g/>{b:2}</style><style>te<![CDATA[xt]]>{c:3}</style></svg>`)

	// entities
	add("entities-predefined", head+`<text a="&lt;&gt;&amp;&apos;&quot;" b='&quot;x&apos;'>&lt;&gt;&amp;&apos;&quot; &amp;amp; &amp;lt;</text></svg>`)
	add("entities-numeric", head+`<text a="&#65;&#x41;&#X41;&#0;&#x0;&#xD800;&#xDFFF;&#x10FFFF;&#x110000;&#99999999999;&#xFFFFFFFFF;&#-1;&#+65;&#;&#x;&#65&#x41 ;&# 65;&#0065;&#x00000041;&#x1F600;&#13;&#10;&#9;">&#65;&#x41;&#X41;&#0;&#xD800;&#x110000;&#;&#x;&#00000000000000000000000000000000000000000000000000000000000065;&#000000000000000000000000000000000000000000000000000000000000065;</text></svg>`)
	add("entities-html", head+`<text a="&nbsp;&copy;&eacute;&hellip;&euro;&NotAnEntity;&Eacute;&EACUTE;&thetasym;&thetasymb;&amp;nbsp;">&nbsp;&copy;&AElig;&zwnj;&zwj;&lrm;&rlm;&frac12;&sup2;&bogus;&;&a b;&nbsp&nbsp ;x&quot;&apos;</text></svg>`)
	add("entities-bare", head+`<text a="& &; &amp &a;b &&& &;&; a&b=c&d; &#;">& &; &amp &&& &lt &gt; &x`+strings.Repeat("y", 70)+`; &`+strings.Repeat("z", 64)+`; &`+strings.Repeat("z", 63)+`; &nbsp`+strings.Repeat(" ", 70)+`;</text></svg>`)
	add("entities-doctype", `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd" [
	<!ENTITY ns_svg "http://www.w3.org/2000/svg">
	<!ENTITY ns_xlink "http://www.w3.org/1999/xlink">
	<!ENTITY st0 "fill:#FF0000;stroke:#000000;">
	<!ENTITY a "&#x41;&lt;&amp;">
	<!ENTITY b "&a;&a;">
	<!ENTITY cr "x&#13;y&#10;z
w">
	<!ENTITY amp2 "&&&;">
	<!ENTITY nbsp "not a space">
	<!ENTITY lt "not less">
	<!ENTITY a "second">
	<!ENTITY % param "ignored">
	<!ENTITY ext SYSTEM "file:///etc/passwd">
	<!ENTITY pub PUBLIC "x" "y">
	<!ENTITY 'q' "quoted name">
	<!ENTITY empty "">
	<!ENTITY single 'single "quoted"'>
	<!ENTITY é "e acute">
	<!ENTITY a.b-c_d:e "punct">
	<!-- <!ENTITY c "in a comment"> -->
	<?pi <!ENTITY d "in a pi"> ?>
	<!ELEMENT svg ANY>
	<!ATTLIST svg x CDATA "]>">
	<!ENTITY after "after the attlist">
]>
<svg xmlns="&ns_svg;" xmlns:xlink="&ns_xlink;" style="&st0;" a="&a;" b="&b;" cr="&cr;" amp2="&amp2;" nbsp="&nbsp;" lt="&lt;" param="&param;" ext="&ext;" pub="&pub;" q="&q;" empty="[&empty;]" single="&single;" e="&é;" punct="&a.b-c_d:e;" c="&c;" d="&d;" after="&after;" und="&undeclared;">
<text>&a;|&b;|&cr;|&amp2;|&nbsp;|&lt;|&empty;|&single;|&é;|&after;|&undeclared;|&copy;</text></svg>`)
	add("entities-doctype-odd", `<!DOCTYPE svg [<!ENTITY a "1"> <!ENTITY b "2" ]> <!ENTITY c "3"> ]><svg a="&a;" b="&b;" c="&c;"/>`)
	add("entities-doctype-quotes", `<!DOCTYPE svg SYSTEM "a>b[c]" [<!ENTITY a "]>"><!ENTITY b '>'>]><svg a="&a;" b="&b;"/>`)
	add("entities-doctype-twice", `<!DOCTYPE svg [<!ENTITY a "1">]><!DOCTYPE svg [<!ENTITY a "2"><!ENTITY b "3">]><svg a="&a;" b="&b;"><!DOCTYPE x [<!ENTITY c "4">]><g c="&c;"/></svg>`)
	add("entities-doctype-unterminated", `<!DOCTYPE svg [<!ENTITY a "1"><svg a="&a;"/>`)
	add("entities-doctype-unterminated-2", `<svg><!DOCTYPE svg [<!ENTITY a "1"`)
	add("entities-doctype-empty", `<!DOCTYPE><!DOCTYPE svg><!DOCTYPE svg []><!DOCTYPE svg [ ]><svg/>`)
	add("entities-doctype-lower", `<!doctype svg [<!ENTITY a "1">]><svg a="&a;"/>`)
	add("entities-doctype-after-use", `<svg a="&a;"><!DOCTYPE svg [<!ENTITY a "1">]><g a="&a;"/></svg>`)
	add("entities-outside-doctype", `<svg><!ENTITY a "`+strings.Repeat("x", 1000)+`"><g a="&a;"/></svg>`)
	add("entities-too-large", `<!DOCTYPE svg [<!ENTITY a "`+strings.Repeat("x", 100000)+`">]><svg>`+strings.Repeat(`<g a="&a;"/>`, 11)+`</svg>`)
	add("entities-just-fit", `<!DOCTYPE svg [<!ENTITY a "`+strings.Repeat("x", 1<<16)+`">]><svg>`+strings.Repeat(`<g a="&a;"/>`, 16)+`</svg>`)
	add("entities-just-over", `<!DOCTYPE svg [<!ENTITY a "`+strings.Repeat("x", 1<<16)+`">]><svg>`+strings.Repeat(`<g a="&a;"/>`, 16)+`<text>&a</text><g b="&a;"/></svg>`)
	add("entities-laughs", `<!DOCTYPE svg [<!ENTITY a "ha"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;"><!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;">]><svg><text>&c;&c;&c;</text></svg>`)
	add("entities-ampersands", `<!DOCTYPE svg [<!ENTITY a "`+strings.Repeat("&", 3000)+`"><!ENTITY r "`+strings.Repeat("&#13;", 500)+`">]><svg><text a="&a;&r;">&a;&a;&r;</text></svg>`)
	add("entities-many-ampersands", head+`<text a="`+strings.Repeat("&", 5000)+`">`+strings.Repeat("&", 5000)+strings.Repeat("&#65;&;&x", 300)+`</text></svg>`)
	// a megabyte of entity text at most by the reference's count, and more
	// than four times the document in one token (a '&' that is no reference
	// is written as five bytes there): the reading ends at the token. The
	// ';' and the line end keep the reference's searches short.
	amps := strings.Repeat(strings.Repeat("&", 30)+";\r", 6000)
	add("entities-token-too-large", `<!DOCTYPE svg [<!ENTITY a "`+amps+`">]><svg><g id="before"/><g a="&a;" b="&a;&a;"/><g id="after"/></svg>`)
	add("entities-text-too-large", `<!DOCTYPE svg [<!ENTITY a "`+amps+`">]><svg><text id="before">t</text><text>&a;&a;&a;</text><g id="after"/></svg>`)
	add("entities-token-large", `<!DOCTYPE svg [<!ENTITY a "`+amps[:16000]+`">]><svg><g a="&a;"/><text>&a;</text><g b="&a;"/></svg>`)
	add("entities-long-names", `<!DOCTYPE svg [<!ENTITY `+strings.Repeat("n", 64)+` "64"><!ENTITY `+strings.Repeat("m", 65)+` "65">]><svg a="&`+strings.Repeat("n", 64)+`;" b="&`+strings.Repeat("m", 65)+`;"/>`)

	// CDATA, comments, processing instructions
	add("cdata", head+`<text><![CDATA[<not a tag> &amp; ]] ]>]]><![CDATA[]]><![CDATA[a`+"\r\nb\rc\n"+`]]>]]&gt;<tspan><![CDATA[x]]></tspan></text><g><![CDATA[dropped]]></g><![CDATA[unterminated</svg>`)
	add("comments", `<!-- before --><svg><!-- in --><text>a<!-- b -->c<!----><!-- -- --><!-- > --></text><!-- unterminated </svg>`)
	add("comments-odd", `<svg><!-><!--></svg>`)
	add("bang", `<svg><!x></svg>`)
	add("pis", `<?xml version="1.0"?><?xml-stylesheet href="x.css" type="text/css"?><svg><?php echo "</svg>" ?><text>a<?b c?>d</text><?xml?><?xml ?><? x?><?></svg>`)
	add("pi-unterminated", `<svg><text>a<?b c</text></svg>`)
	add("pi-xml-names", `<svg><?xml:x encoding="latin1"?><?XML encoding="latin1"?><?xmlx encoding="latin1"?><g a="`+"\xe9"+`"/></svg>`)

	// namespaces and prefixes
	add("prefixes", `<svg:svg xmlns:svg="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="x" foo:xmlns="y" xmlns:="z" :xmlns="w" xmlnsx="v">
<svg:use xlink:href="#a" href="#b"/><use href="#b" xlink:href="#a"/><use xlink:href="#a" foo:href="#c"/><use foo:href="#c" xlink:href="#a" bar:href="#d"/><use href="#1" href="#2" xlink:href="#3" href="#4"/>
<a:b:c d:e:f="1" :g="2" h:="3" : ="4" ::="5" i:j="6" j="7" k:j="8"/><:x/><y:/><:/><svg:text svg:x="1" x="2">t</svg:text><x:style>rect{a:1}</x:style><x:tspan>dropped</x:tspan></svg:svg>`)
	add("prefix-not-svg", `<svg:g xmlns:svg="http://www.w3.org/2000/svg"><svg/></svg:g>`)

	// the void elements of HTML, attributes without values or quotes
	add("html", head+`<foreignObject width="100" height="50"><p>one<br>two<BR>three<br/>four<br></br>five<hr><img src="x.png" alt=x><input type=checkbox checked disabled>
<meta charset="utf-8"><link rel=stylesheet><area><base><basefont><col><frame><isindex><param name=a value=b><xhtml:br><XHTML:IMG><br ></br ><br></BR><br></ br></p></foreignObject></svg>`)
	add("html-void-end", head+`<foreignObject><br>`)
	add("html-void-end-2", head+`<foreignObject><br></b`)
	add("html-void-end-3", head+`<foreignObject><br></br`)
	add("html-void-end-4", head+`<foreignObject><br></brx></br></foreignObject></svg>`)
	add("html-void-nested", head+`<br><g/></br><img><g/></img><param></param><g/></svg>`)
	add("unquoted", `<svg a=b c = d e= f g =h i=j-k:l_m n=o.p q=r/s t=>`+`<g u v=w x/><g y=/><g z=''=""/><g a="1"b="2"/><g a='x"y' b="x'y"/></svg>`)
	add("unquoted-end", `<svg a=b`)
	add("unquoted-bad", `<svg a=<b/>`)
	add("attr-odd", `<svg a="1" a="2" A="3" a.b="4" a-b="5" 1="6" é="7" `+"\xff=\"8\" x\x00y=\"9\""+` a="10"><g a="`+"line\r\none\rtwo\nthree\tfour"+`" b="  padded  " c="" d='' e="&#10;&#13;"/></svg>`)
	add("attr-slash", `<svg><g a="1"/ b="2"/></svg>`)
	add("attr-lt", `<svg><g a="<b>" c='<d>'/><g e="1"<f/></svg>`)

	// unclosed and misnested elements
	add("misnested", `<svg><g><rect></g><circle></svg><after/>`)
	add("misnested-2", `<svg><a><b><c></a></c></b></svg>`)
	add("misnested-deep", `<svg><a><b><c><d></a><e/></svg>`)
	add("stray-end", `<svg></g><rect/></svg>`)
	add("stray-end-first", `</svg><svg/>`)
	add("end-space", `<svg ><g ></g ><rect /></svg >`)
	add("end-attr", `<svg><g></g a="b"><rect/></svg>`)
	add("end-case", `<svg><G></g><rect/></svg>`)
	add("end-hash-collision", `<svg><costarring><g/></liquid><rect/><declinate><g/></macallums><circle/></svg>`)
	add("unclosed", `<svg><g><rect>`)
	add("unclosed-text", `<svg><text>abc`)
	add("unclosed-tag", `<svg><g a="1" b`)
	add("unclosed-value", `<svg><g a="1`)
	add("unclosed-name", `<svg><g`)
	add("empty-name", `<svg>< g/><rect/></svg>`)
	add("empty-end-name", `<svg></ ><rect/></svg>`)
	add("lt-at-end", `<svg><`)
	add("lt-slash-at-end", `<svg></`)
	add("second-root", `<svg id="a"><g id="b"/></svg><svg id="c"><g id="b"/><g id="d"><text id="e">t<tspan id="f">u</tspan></text></g></svg><other id="g"><style>g{a:1}</style></other><svg id="a"/>`)
	add("text-outside", `text before<svg>inside</svg>text after<text>after</text>`)
	add("not-svg", `<html><body><svg/></body></html>`)
	add("not-svg-case", `<SVG/>`)
	add("no-root", `<!-- nothing -->`)
	add("only-text", `just text`)
	add("only-space", " \n\t ")
	add("empty", ``)
	add("self-closed-root", `<svg/>`)
	add("root-name", `<svg x/><y/>`)
	add("nul", "<svg a=\"x\x00y\"><text>a\x00b</text>\x00</svg>")

	// runs of text in and out of the elements that draw them
	add("text-runs", head+`  <text>  one  <tspan> two </tspan>  three<a> four <tspan/>five</a><textPath>six</textPath>
</text><g> dropped <text/> dropped </g><a>kept</a><tspan>kept</tspan><textPath>kept</textPath><title>dropped</title><desc>dropped</desc><rect>dropped</rect>
<text>a<g>dropped</g>b<text>nested</text>c</text><TEXT>dropped</TEXT><svg:text>kept</svg:text><text>`+"cr\r\nlf\rx\ny &#13; z"+`</text></svg>`)

	// an element named as the runs of text are: one of them to the reference
	add("text-named-element", head+`<style>g{a:1} rect{b:2} *{c:3} #t{d:4} #u{e:5}</style><#text id="t" x="1" style="fill:red"><g id="u"><rect/></g>inner</#text><g/><text>run<#text a="b"/>more<#text>x</#text></text><x:#text/></svg>`)
	add("text-named-root", `<#text><svg/></#text>`)

	// encodings
	body := `<svg><text a="é€ñ">café €5 — “quoted” 日本語 𝄞</text></svg>`
	add("utf8-mark", "\xef\xbb\xbf"+body)
	add("utf8-two-marks", "\xef\xbb\xbf\xef\xbb\xbf"+body)
	add("utf8-three-marks", "\xef\xbb\xbf\xef\xbb\xbf\xef\xbb\xbf"+body)
	add("utf8-mark-only", "\xef\xbb\xbf")
	add("utf8-two-marks-only", "\xef\xbb\xbf\xef\xbb\xbf")
	add("utf8-mark-declared", "\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"UTF-8\"?>"+body)
	add("utf8-declared", `<?xml version="1.0" encoding="UTF-8" standalone="no"?>`+body)
	add("utf8-declared-single", `<?xml version='1.0' encoding='utf-8'?>`+body)
	add("utf8-invalid", "<svg a=\"\xff\xfe\xfd x \xc3 y \xe2\x82 z \xf0\x9f\x98 w \xed\xa0\x80 v \xc0\xaf u \xf4\x90\x80\x80\"><text>\x80\x81\xff ok \xe9t\xe9</text></svg>")
	add("utf8-invalid-declared", "<?xml version=\"1.0\" encoding=\"utf-8\"?><svg a=\"\xff\xfe x \xc3\"><text>\x80\x81 caf\xe9</text></svg>")
	add("utf8-label-utf8", "<?xml version=\"1.0\" encoding=\"utf8\"?><svg a=\"\xff\xfe\xfd x \xc3 y \xe2\x82 z \xf0\x9f\x98 w \xed\xa0\x80 v \xc0\xaf u \xf4\x90\x80\x80 t \xe2\x82\"><text>\x80\x81\xff caf\xe9 \xf0\x9f</text></svg>\xe2\x82")
	add("utf8-label-padded", "<?xml version=\"1.0\" encoding=\" UTF-8 \"?><svg a=\"\xff\xff\"><text>\xff\xfe</text></svg>")
	add("utf8-label-unicode", "<?xml version=\"1.0\" encoding=\"unicode-1-1-utf-8\"?><svg a=\"\xff\xff\"/>")
	for _, be := range []bool{false, true} {
		name := map[bool]string{false: "le", true: "be"}[be]
		add("utf16"+name+"-mark", string(utf16Of(body, be, true)))
		add("utf16"+name+"-mark-declared", string(utf16Of(`<?xml version="1.0" encoding="UTF-16"?>`+body, be, true)))
		add("utf16"+name+"-mark-declared-other", string(utf16Of(`<?xml version="1.0" encoding="shift_jis"?>`+body, be, true)))
		add("utf16"+name+"-mark-declared-unknown", string(utf16Of(`<?xml version="1.0" encoding="no-such"?>`+body, be, true)))
		add("utf16"+name+"-mark-declared-utf8", string(utf16Of(`<?xml version="1.0" encoding="utf-8"?>`+body, be, true)))
		add("utf16"+name+"-mark-declared-twice", string(utf16Of(`<?xml version="1.0" encoding="UTF-16"?><svg><?xml version="1.0" encoding="UTF-16"?><g/><?xml?><?xml encoding="x"?><h/></svg>`, be, true)))
		add("utf16"+name+"-mark-odd", string(utf16Of(body, be, true))+"x")
		add("utf16"+name+"-mark-surrogates", string(utf16Of("<svg a=\"", be, true))+"\xd8\x00\x00\xd8\xdc\x00\x00\xdc\xdc\x00\xdc\x00\xd8\x00\xd8\x00"+string(utf16Of("\"/>", be, false)))
		add("utf16"+name+"-mark-mark", string(utf16Of("\ufeff"+body, be, true)))
		add("utf16"+name+"-mark-entities", string(utf16Of(`<!DOCTYPE svg [<!ENTITY a "`+strings.Repeat("x", 1000)+`">]><svg a="&a;&a;"/>`, be, true)))
		add("utf16"+name+"-declared", `<?xml version="1.0" encoding="utf-16`+name+`"?>`+string(utf16Of(body, be, false)))
		add("utf16"+name+"-declared-mark", `<?xml version="1.0" encoding="utf-16`+name+`"?>`+string(utf16Of(body, be, true)))
		add("utf16"+name+"-declared-odd", `<?xml version="1.0" encoding="utf-16`+name+`"?>`+string(utf16Of(body, be, false))+"x")
		add("utf16"+name+"-declared-surrogates", `<?xml version="1.0" encoding="utf-16`+name+`"?>`+string(utf16Of("<svg a=\"", be, false))+"\xd8\x00\x00\xd8\xdc\x00\x00\xdc\xdc\x00\xdc\x00\xd8\x00\xd8\x00\xdc\x00\xd8\x00\xdc"+string(utf16Of("\"/>", be, false))+"\xd8")
	}
	add("utf16-declared", `<?xml version="1.0" encoding="utf-16"?>`+string(utf16Of(body, false, false)))
	add("utf16-declared-labels", `<?xml version="1.0" encoding="ucs-2"?>`+string(utf16Of(body, false, false)))
	add("utf16-declared-unicodefffe", `<?xml version="1.0" encoding="unicodeFFFE"?>`+string(utf16Of(body, true, false)))
	add("utf16-mark-only", "\xff\xfe")
	add("utf16-mark-and-a-byte", "\xfe\xff<")
	add("utf16-after-utf8-mark", "\xef\xbb\xbf\xff\xfe<\x00s\x00v\x00g\x00/\x00>\x00")
	add("ff-ff", "\xff\xff<svg/>")
	high := "<svg a=\"\x80\x81\x82\x8d\x8f\x90\x9d\x9f\xa0\xe9\xff\"><text>caf\xe9 \x93quoted\x94 \x80 5</text></svg>"
	for _, label := range []string{"ISO-8859-1", "iso-8859-1", "latin1", "Latin1", "L1", "windows-1252", "cp1252", "us-ascii", "ASCII", "ansi_x3.4-1968", "ibm819", "x-cp1252", "csisolatin1"} {
		add("latin1-"+label, `<?xml version="1.0" encoding="`+label+`"?>`+high)
	}
	for _, label := range []string{"shift_jis", "Shift-JIS", "euc-jp", "gbk", "gb18030", "big5", "euc-kr", "koi8-r", "iso-8859-2", "iso-8859-15", "windows-1251", "macintosh", "x-user-defined", "replacement", "iso-2022-jp", "ibm866", "hz-gb-2312"} {
		add("other-"+label, `<?xml version="1.0" encoding="`+label+`"?>`+high)
		add("other-after-root-"+label, `<svg><g/><?xml version="1.0" encoding="`+label+`"?><rect/></svg>`)
	}
	for _, label := range []string{"no-such", "", " ", "utf-16x", "latin 1", "iso-8859-1 x", "utf-7", "ucs-4", "utf-32", "x", "\xff", "latin1\x00"} {
		add("unknown-"+fmt.Sprintf("%q", label), `<?xml version="1.0" encoding="`+label+`"?>`+high)
		add("unknown-after-root-"+fmt.Sprintf("%q", label), `<svg><g/><?xml version="1.0" encoding="`+label+`"?><rect/></svg>`)
	}
	for i, label := range []string{" latin1", "latin1 ", "\tLATIN1\n", "\u00a0latin1\u00a0", "\u2003latin1\u3000", "lat\u0130n1", "LAT\u0130N1", "\u212aoi8-r", "\u212AOI8-R", "utf\u00a0-8", "&#108;atin1", "latin&#49;", "latin1&#32;", "l&amp;1", "\r\nlatin1\r\n", "latin1&#13;", "latin\xff1", "\xfflatin1", "ｌatin1", "UTF8", "Utf-8 ", "uTf-16Le", "UTF-16BE"} {
		add(fmt.Sprintf("label-%d", i), `<?xml version="1.0" encoding="`+label+`"?>`+high)
	}
	add("declared-not-first", ` <?xml version="1.0" encoding="latin1"?>`+high)
	add("declared-after-comment", `<!-- c --><?xml version="1.0" encoding="latin1"?>`+high)
	add("declared-after-root", `<svg a="`+"\xe9"+`"><?xml version="1.0" encoding="latin1"?><g a="`+"\xe9"+`"/></svg>`)
	add("declared-after-doctype", `<!DOCTYPE svg [<!ENTITY a "`+"\xe9"+`"><!ENTITY `+"\xc3\xa9"+` "utf">]><?xml version="1.0" encoding="latin1"?><svg a="&a;" b="&`+"\xe9"+`;" c="`+"\xe9"+`"/>`)
	add("declared-twice", `<?xml version="1.0" encoding="latin1"?><svg a="`+"\xe9"+`"><?xml version="1.0" encoding="latin1"?><g a="`+"\xe9"+`"/></svg>`)
	add("declared-twice-hidden", `<?xml version="1.0" encoding="latin1"?><svg a="`+"\xe9"+`"><!-- <?xml version="1.0" encoding="latin1"?> --><![CDATA[<?xml encoding="x"?>]]><g a="`+"\xe9"+`"/><!-- unterminated <?xml version="1.0" encoding="latin1"?><h/></svg>`)
	add("declared-twice-reached", `<?xml version="1.0" encoding="latin1"?><svg a="`+"\xe9"+`"><g b="<!--"/><?xml version="1.0" encoding="latin1"?><g a="`+"\xe9"+`"/></svg>`)
	add("declared-twice-tab", `<?xml version="1.0" encoding="latin1"?><svg><?xml`+"\t"+`encoding="latin1"?><g/><?xml`+"\r"+`encoding="x"?><h/><?xml`+"\n"+`encoding="x"?><i/></svg>`)
	add("declared-twice-nospace", `<?xml version="1.0" encoding="latin1"?><svg><?xml?><g/><?xml/encoding="x"?><h/></svg>`)
	add("declared-no-version", `<?xml encoding="latin1"?>`+high)
	add("declared-spaces", `<?xml version = "1.0"   encoding  =  "latin1"  ?>`+high)
	add("declared-unquoted-version", `<?xml version=1.0 encoding="latin1"?>`+high)
	add("declared-unquoted", `<?xml version="1.0" encoding=latin1?>`+high)
	add("declared-unterminated-value", `<?xml version="1.0" encoding="latin1?>`+high)
	add("declared-in-other-value", `<?xml foo="encoding" encoding="latin1"?>`+high)
	add("declared-in-other-value-2", `<?xml foo=" encoding='utf-8' " encoding="latin1"?>`+high)
	add("declared-other-name", `<?xmlfoo version="1.0" encoding="latin1"?>`+high)
	add("declared-case", `<?XML version="1.0" encoding="latin1"?>`+high)
	add("declared-encoding-case", `<?xml version="1.0" ENCODING="latin1"?>`+high)
	add("declared-empty-pi", `<?xml?>`+high)
	add("declared-long", `<?xml version="1.0" `+strings.Repeat(" ", 300)+`encoding="latin1"?>`+high)
	add("declared-late", strings.Repeat(" ", 1100)+`<?xml version="1.0" encoding="latin1"?>`+high)
	add("declared-unterminated", `<?xml version="1.0" encoding="latin1"`+high)
	add("declared-then-nothing", `<?xml version="1.0" encoding="latin1"?>`)
	add("declared-utf16-then-nothing", `<?xml version="1.0" encoding="utf-16"?>`)
	add("declared-entities-hidden", `<!DOCTYPE svg [<!ENTITY a "`+strings.Repeat("x", 300)+`">]><?xml version="1.0" encoding="utf-16le"?>`+string(utf16Of(`<svg a="&a;&a;" b="&a;"><text>&a;</text></svg>`, false, false)))
	add("latin1-entities", `<?xml version="1.0" encoding="latin1"?><!DOCTYPE svg [<!ENTITY a "`+"\xe9\x80"+`"><!ENTITY `+"\xe9"+` "name">]><svg a="&a;" b="&`+"\xe9"+`;"><text>&eacute;&a;</text></svg>`)
	add("latin1-xml-in-rest", `<?xml version="1.0" encoding="latin1"?><svg><?xmlx y?><text>&lt;?xml </text><?xml version="1.0"?><g a="<?xml "/></svg>`)

	// limits: depth, nodes, style matches (with the small limits of the tests)
	deep := func(n int) string {
		return strings.Repeat("<g>", n) + "<rect/>" + strings.Repeat("</g>", n)
	}
	for _, n := range []int{38, 39, 40, 41, 254, 255, 256, 257, 300} {
		add(fmt.Sprintf("depth-%d", n), `<svg>`+deep(n)+`<circle/></svg>`)
	}
	add("depth-text", `<svg>`+strings.Repeat("<text>", 39)+`deep`+strings.Repeat("</text>", 39)+`</svg>`)
	add("depth-void", `<svg>`+strings.Repeat("<g>", 38)+`<br><br/><g><br></g>`+strings.Repeat("</g>", 38)+`</svg>`)
	for _, n := range []int{1996, 1997, 1998, 1999, 2000, 2001} {
		add(fmt.Sprintf("nodes-%d", n), `<svg>`+strings.Repeat("<g/>", n)+`<text>t</text><style>g{a:1}</style></svg>`)
		add(fmt.Sprintf("nodes-text-%d", n), `<svg><text>`+strings.Repeat("a<tspan/>", n/2)+`b<tspan>c</tspan>d</text><g/></svg>`)
	}
	add("matches", `<svg><style>`+strings.Repeat("g{a:1}", 100)+`rect{b:2}.c{c:3}*{d:4}</style>`+strings.Repeat(`<g class="c"/>`, 210)+`<rect class="c"/><g/></svg>`)
	add("matches-exact", `<svg><style>`+strings.Repeat("g{a:1}", 100)+`</style>`+strings.Repeat(`<g/>`, 200)+`</svg>`)
	add("matches-one-over", `<svg><style>`+strings.Repeat("g{a:1}", 100)+`</style>`+strings.Repeat(`<g/>`, 200)+`<g/><g/></svg>`)
	add("matches-nested", `<svg><style>g g{a:1} g{b:2} #x{c:3} .k{d:4}</style>`+strings.Repeat(`<g id="x" class="k k"><g>`, 30)+strings.Repeat(`</g></g>`, 30)+strings.Repeat(`<g class="k"/>`, 6000)+`</svg>`)

	// many attributes, many classes, long values
	var many strings.Builder
	many.WriteString("<svg")
	for i := 0; i < 300; i++ {
		fmt.Fprintf(&many, ` a%d="%d" a%d="x"`, i%200, i, (i*7)%300)
	}
	many.WriteString(` xlink:href="a" foo:href="b"><style>`)
	for i := 0; i < 50; i++ {
		fmt.Fprintf(&many, ".c%d{p%d:%d;q:%d} ", i, i%7, i, i)
	}
	many.WriteString(`</style><g class="`)
	for i := 0; i < 120; i++ {
		fmt.Fprintf(&many, "c%d ", (i*13)%60)
	}
	many.WriteString(`" style="`)
	for i := 0; i < 200; i++ {
		fmt.Fprintf(&many, "p%d: %d; ", (i*11)%150, i)
	}
	many.WriteString(`"/></svg>`)
	add("many", many.String())
	add("long-value", `<svg d="`+strings.Repeat("M 0 0 L 10.5,20.25 ", 5000)+`"><text>`+strings.Repeat("long text ", 5000)+`</text></svg>`)

	// every named character reference of HTML, and every colour and unit as a value
	var ents []string
	for name := range xml.HTMLEntity {
		ents = append(ents, name)
	}
	sort.Strings(ents)
	var eb strings.Builder
	eb.WriteString("<svg><text>")
	for _, name := range ents {
		fmt.Fprintf(&eb, "&%s;|&%s|&%sx;|&%s;", name, name, name, strings.ToUpper(name))
	}
	eb.WriteString("</text></svg>")
	add("html-entities", eb.String())
	return docs
}

func TestBuiltDocuments(t *testing.T) {
	docs := builtDocs()
	checkDocs(t, docs)
	// the documents the C reader refuses on purpose are known by name
	for name, data := range docs {
		c := cDump(data, nil, false, false)
		// an encoding that is not read, declared where a declaration counts
		// (before the root element), and bytes of it
		other := strings.HasPrefix(name, "other-") && !strings.HasPrefix(name, "other-after-root-") || name == "label-7" || name == "label-8"
		if (c == "encoding\n") != other {
			t.Errorf("%s: the C reader says %q", name, strings.SplitN(c, "\n", 2)[0])
		}
	}
}

// TestStatuses checks that the documents made for a status have it in both
// readers.
func TestStatuses(t *testing.T) {
	docs := builtDocs()
	for name, want := range map[string]string{
		"renderer": "doc", "css-combinators": "doc", "css-combinators-2": "doc", "css-combinators-space": "doc", "css-combinators-not-svg": "notsvg", "css-combinators-beside": "doc",
		"not-svg": "notsvg", "not-svg-case": "notsvg", "no-root": "notsvg", "empty": "notsvg", "prefixes": "doc", "prefix-not-svg": "notsvg",
		"unclosed": "doc", "stray-end-first": "notsvg", "utf16le-mark": "doc", "utf16be-declared": "doc", "latin1-latin1": "doc",
		"unknown-\"no-such\"": "notsvg", "unknown-after-root-\"no-such\"": "doc", "entities-too-large": "doc",
	} {
		data, ok := docs[name]
		if !ok {
			t.Fatalf("no document %s", name)
		}
		for _, d := range []string{goDump(data, nil, false, false), cDump(data, nil, false, false)} {
			if got := strings.SplitN(d, "\n", 2)[0]; got != want {
				t.Errorf("%s: %s, want %s", name, got, want)
			}
		}
	}
	// a limit that is passed is a warning in both
	for name, want := range map[string]string{
		"depth-39":         "W\tthe elements of an SVG image are more than 40 deep: the rest are not read\n",
		"nodes-1997":       "W\tan SVG image has more than 2000 elements: the rest are not read\n",
		"matches-one-over": "W\tthe style sheets of an SVG image have too many rules for its elements: the rest of the elements are drawn without them\n",
	} {
		for _, d := range []string{goDump(docs[name], smallLimits, false, false), cDump(docs[name], smallLimits, false, false)} {
			if !strings.HasSuffix(d, want) {
				t.Errorf("%s: no warning %q", name, want)
			}
		}
	}
	for _, name := range []string{"depth-38", "nodes-1996", "matches-exact"} {
		if d := cDump(docs[name], smallLimits, false, false); strings.Contains(d, "\nW\t") {
			t.Errorf("%s: a warning", name)
		}
	}
	_ = svg.DefaultMaxDepth
}

func TestFiles(t *testing.T) {
	checkDocs(t, svgFiles(t))
}

// allDocs is the documents that are damaged and that the fuzzers start
// from: the files and the documents the tests build, but for the two built
// of 190,000 ampersands, whose damaged copies take the reference seconds
// each (its Unescape looks through the rest of a text from every '&').
func allDocs(t testing.TB) map[string][]byte {
	docs := svgFiles(t)
	for name, data := range builtDocs() {
		if len(data) < 160000 {
			docs["built/"+name] = data
		}
	}
	return docs
}

func TestMutations(t *testing.T) {
	checkMutations(t, allDocs(t))
}

// TestKnownDifferences shows the documents the C reader refuses and the
// reference reads: what NOTES.md lists.
func TestKnownDifferences(t *testing.T) {
	status := func(d string) string { return strings.SplitN(d, "\n", 2)[0] }
	check := func(name string, doc []byte, want string) {
		t.Helper()
		g, c := status(goDump(doc, nil, false, false)), status(cDump(doc, nil, false, false))
		if g != "doc" || c != want {
			t.Errorf("%s: the reference says %s, the C reader %s (want doc and %s)", name, g, c, want)
		}
	}
	// an encoding the C reader does not read
	check("shift_jis", []byte(`<?xml version="1.0" encoding="shift_jis"?><svg><text>`+"\x93\xfa\x96\x7b"+`</text></svg>`), "encoding")
	check("iso-2022-jp", []byte(`<?xml version="1.0" encoding="iso-2022-jp"?><svg><text>`+"\x1b$B\x46\x7c\x1b(B"+`</text></svg>`), "encoding")
	// read alike: the same encodings with nothing but ASCII after the
	// declaration, and what looks like a declaration inside the root
	// element, which is a processing instruction and converts nothing
	for _, doc := range []string{`<?xml version="1.0" encoding="shift_jis"?><svg><text>ascii</text></svg>`, `<svg><?xml version="1.0" encoding="windows-1251"?><g/></svg>`,
		`<?xml version="1.0" encoding="iso-2022-jp"?><svg><text>ascii</text></svg>`, `<?xml version="1.0" encoding="gb18030"?><svg a="&#x65e5;">&amp;</svg>`,
		`<svg><?xml version="1.0" encoding="shift_jis"?><text>` + "\x93\xfa" + `</text></svg>`, `<svg><?xml version="1.0" encoding="iso-2022-kr"?></svg>`,
		`<?xml version="1.0"?><?xml version="1.0" encoding="shift_jis"?><svg><text>` + "\x93\xfa" + `</text></svg>`} {
		if d := compare([]byte(doc), nil); d != "" || !strings.HasPrefix(cDump([]byte(doc), nil, false, false), "doc\n") {
			t.Errorf("%q: %s", doc, d)
		}
	}
	// entities whose references the count before the reading does not see:
	// they are declared before the XML declaration, in ASCII, and referred
	// to after it, in UTF-16. The reader bounds what they add to the
	// document in all, and both stop at the same reference (2.4 MB of text
	// here, in a document of 300 KB).
	hidden := `<!DOCTYPE svg [<!ENTITY a "` + strings.Repeat("x", 300000) + `">]><?xml version="1.0" encoding="utf-16le"?>` +
		string(utf16Of(`<svg><g a="&a;&a;"/><g a="&a;&a;"/><g a="&a;&a;"/><g a="&a;&a;"/></svg>`, false, false))
	if d := compare([]byte(hidden), nil); d != "" || strings.Count(cDump([]byte(hidden), nil, false, false), "\nE\t") >= 5 {
		t.Errorf("hidden entities: %s", d)
	}
	// the same within the bound is read whole
	small := strings.Replace(hidden, strings.Repeat("x", 300000), strings.Repeat("x", 100000), 1)
	if d := compare([]byte(small), nil); d != "" || strings.Count(cDump([]byte(small), nil, false, false), "\nE\t") != 5 {
		t.Errorf("hidden entities within the bound: %s", d)
	}
	if os.Getenv("LIBBDF_SVG_HEAVY") == "" {
		t.Log("LIBBDF_SVG_HEAVY is not set: the documents past the C reader's own limits (style declarations, size) are not made")
		return
	}
	// more declarations kept for the elements than BDFI_SVG_MAX_SHEET_DECLS:
	// 2000 elements that each get 2101 declarations of their own
	var b strings.Builder
	b.WriteString("<svg><style>g{")
	for i := 0; i < 2100; i++ {
		fmt.Fprintf(&b, "p%d:%d;", i, i)
	}
	b.WriteString("}")
	for i := 0; i < 2000; i++ {
		fmt.Fprintf(&b, "#i%d{x:%d}", i, i)
	}
	b.WriteString("</style>")
	for i := 0; i < 2000; i++ {
		fmt.Fprintf(&b, `<g id="i%d"/>`, i)
	}
	b.WriteString("</svg>")
	check("style declarations", []byte(b.String()), "sheets")
	// a document of more than BDFI_SVG_MAX_INPUT bytes
	check("size", []byte("<svg>"+strings.Repeat(" ", 1<<27)+"</svg>"), "input")
}
