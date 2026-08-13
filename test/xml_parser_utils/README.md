# OPF QName Regression

`XmlParserUtilsTest` covers the element-name behavior that keeps CrossPoint from showing "End of book" for valid
EPUBs whose OPF package uses arbitrary namespace prefixes.

A manual regression EPUB should use this OPF shape with a Lorem Ipsum chapter:

```xml
<ns0:package xmlns:ns0="http://www.idpf.org/2007/opf"
             xmlns:dc="http://purl.org/dc/elements/1.1/"
             version="2.0"
             unique-identifier="bookid">
  <ns0:metadata>
    <dc:title>Lorem Ipsum QName Test</dc:title>
    <dc:creator>CrossPoint Test</dc:creator>
    <dc:language>en</dc:language>
  </ns0:metadata>
  <ns0:manifest>
    <ns0:item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </ns0:manifest>
  <ns0:spine>
    <ns0:itemref idref="chapter1"/>
  </ns0:spine>
</ns0:package>
```

On device, delete that book's `.crosspoint` cache before reopening it. The reader should render the Lorem Ipsum
chapter instead of immediately showing the end-of-book screen.
