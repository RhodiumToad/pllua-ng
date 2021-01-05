<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">

  <xsl:output method="html" version="5.0" doctype-system="about:legacy-compat" />

  <!-- any variables we might want to use later -->
  <xsl:variable name="canon_url">https://pllua.github.io/pllua/</xsl:variable>
  <xsl:variable name="canon_site">https://github.com/pllua/pllua/</xsl:variable>

  <!--
      general purpose utility templates
  -->

  <!--
      copy-add-class(newclass, [attr], [copyattr], [copychild])

      Adds string "newclass" to a class-like (i.e. space-delimited)
      attribute named "attr" of the current node, which defaults to
      "class" if not specified. The result is applied to the current
      result node. If copyattr is not specified as false, copies all
      other attributes too; if copychild is not false, copy the child
      elements.
  -->
  <xsl:template name="copy-add-class">
    <xsl:param name="newclass" />
    <xsl:param name="attr" select="string('class')" />
    <xsl:param name="copyattr" select="true()" />
    <xsl:param name="copychild" select="true()" />
    <xsl:if test="$copyattr">
      <xsl:apply-templates select="@*" />
    </xsl:if>
    <xsl:attribute name="{$attr}">
      <xsl:value-of select="normalize-space(concat(string(attribute::*[name() = $attr]),' ',$newclass))"/>
    </xsl:attribute>
    <xsl:if test="$copychild">
      <xsl:apply-templates />
    </xsl:if>
  </xsl:template>

  <!--
      We want to munge certain cases of <ul><li>..<br/>..</li></ul>
      from the input into <dl><dt>..</dt><dd>..</dd></dl>, possibly
      with multiple <br/> tags leading to multiple <dt> elements.

      We do this as follows: for all nodes immediately under <li>, we
      tag the node with the id of the nearest following <br/> tag.
      This includes text nodes but the <br/> nodes themselves are
      excluded.

      Then, to output the <li> element, we traverse each child <br/>,
      and collect into a <dt> element all the nodes tagged to that
      <br/>. Then the remaining nodes (distinguished by not having a
      following <br/> sibling) are just processed into the <dd>
      element.

      We end up tagging nodes that we don't actually intend to munge,
      but that's harmless.
  -->

  <xsl:key
      name="dl-br-key"
      match="li/node()[not(self::br) and following-sibling::br]"
      use="generate-id(following-sibling::br[1])"
      />

  <!--
      match the <ul> instances we wish to munge, and enter mode
      "dl-br" for them.
  -->
  <xsl:template match="ul[child::li[1][child::br] and not(parent::div[@class='no-dl-fudge'])]">
    <dl>
      <xsl:call-template name="copy-add-class">
        <xsl:with-param name="newclass" select="string('dl-br')" />
        <xsl:with-param name="copychild" select="false()" />
      </xsl:call-template>
      <xsl:apply-templates mode="dl-br" />
    </dl>
  </xsl:template>

  <!--
      MODE: dl-br

      This is used at the immediate top level under a <ul> we're
      munging; we process the <li> elements specially and copy
      anything else.
  -->

  <xsl:template mode="dl-br" match="li[child::br]">
    <xsl:for-each select="./br">
      <dt>
        <xsl:apply-templates
            select="key('dl-br-key',generate-id())[* or normalize-space(text())]"
            />
      </dt>
    </xsl:for-each>
    <xsl:if test="child::br[position()=last() and (following-sibling::* or normalize-space(following-sibling::text()))]">
      <dd>
        <xsl:apply-templates select="node()[not(following-sibling::br or self::br)]" />
      </dd>
    </xsl:if>
  </xsl:template>

  <!-- default action for this mode is just to punt to the normal mode -->
  <xsl:template mode="dl-br" match="*|@*">
    <xsl:apply-templates select="." />
  </xsl:template>

  <!--
      END MODE: dl-br
  -->

  <!--
      We also want to munge some cases of <ul><li>...</li></ul> where
      the <li> elements contain paragraph text. These also become <dl>
      elements, with the first paragraph as <dt> (losing its <p> tag)
      and all following ones collected inside a <dd> with their <p>
      tags intact.
  -->

  <!--
      match the <ul> instances we wish to munge, and enter mode
      "dl-p" for them.
  -->
  <xsl:template match="ul[child::li[1][child::p] and not(parent::div[@class='no-dl-fudge'])]">
    <dl>
      <xsl:call-template name="copy-add-class">
        <xsl:with-param name="newclass" select="string('dl-p')" />
        <xsl:with-param name="copychild" select="false()" />
      </xsl:call-template>
      <xsl:apply-templates mode="dl-p" />
    </dl>
  </xsl:template>

  <!--
      MODE: dl-p

      Used immediately at top level under a <ul>, we munge the <li>
      elements and copy the rest.
  -->

  <xsl:template mode="dl-p" match="li">
    <dt>
      <xsl:apply-templates select="p[1]/@*" />
      <xsl:apply-templates select="p[1]/node()" />
    </dt>
    <xsl:if test="child::p[following-sibling::p]">
      <dd>
        <xsl:apply-templates select="node()[preceding-sibling::p]" />
      </dd>
    </xsl:if>
  </xsl:template>

  <!-- default action for this mode is just to punt to the normal mode -->
  <xsl:template mode="dl-p" match="*|@*">
    <xsl:apply-templates select="." />
  </xsl:template>

  <!--
      END MODE: dl-p
  -->

  <!--
      External hrefs in <a> tags; add keywords to rel= attribute.
  -->
  <xsl:template match='a[starts-with(@href,"http://") or starts-with(@href,"https://")]'>
    <a>
      <xsl:call-template name="copy-add-class">
        <xsl:with-param name="attr" select="string('rel')" />
        <xsl:with-param name="newclass" select="string('external nofollow')" />
      </xsl:call-template>
    </a>
  </xsl:template>

  <!--
      We want to move "footer" around in the skeleton, so make copying
      it conditional.
  -->
  <xsl:template match="div[@id='footer']">
    <xsl:param name="footer" select="false()" />
    <xsl:if test="$footer">
      <xsl:copy>
        <xsl:call-template name="copy-add-class">
          <xsl:with-param name="newclass" select="string('maincolumn footer')" />
        </xsl:call-template>
      </xsl:copy>
    </xsl:if>
  </xsl:template>

  <!--
      Assign classes to some elements to simplify CSS.
  -->

  <!--
      <code> not inside <pre> is assigned a "shortcode" or "longcode"
      class according to its content length.
  -->
  <xsl:template match="code[not(ancestor::pre)]">
    <xsl:copy>
      <xsl:call-template name="copy-add-class">
        <xsl:with-param name="newclass">
          <xsl:choose>
            <xsl:when test="string-length() &lt; 40">shortcode</xsl:when>
            <xsl:otherwise>longcode</xsl:otherwise>
          </xsl:choose>
        </xsl:with-param>
      </xsl:call-template>
    </xsl:copy>
  </xsl:template>

  <!--
      <pre> containing <code> is a codeblock class.
  -->
  <xsl:template match="pre[child::code]">
    <xsl:copy>
      <xsl:call-template name="copy-add-class">
        <xsl:with-param name="newclass" select="string('codeblock')" />
      </xsl:call-template>
    </xsl:copy>
  </xsl:template>

  <!--
      Table of Contents.

      We generate a structured listing (currently <ol>s) from the flat
      list of <h*> nodes in the document. We do this with another
      tagging trick: we tag every h(N) node with the generated id of
      the h(N-1) node that precedes it in document order. This allows
      us to easily collect the h(N+1) nodes for a given h(N) node.

      Currently the hard limit is <h5>, but a maxdepth parameter is
      used to limit this further.
  -->

  <!--
      keys for Hn nodes
      key name format matters, for incrementing in toc-recurse
  -->
  <xsl:key name="hkey-2" match="h2" use="generate-id(preceding::h1[1])"/>
  <xsl:key name="hkey-3" match="h3" use="generate-id(preceding::h2[1])"/>
  <xsl:key name="hkey-4" match="h4" use="generate-id(preceding::h3[1])"/>
  <xsl:key name="hkey-5" match="h5" use="generate-id(preceding::h4[1])"/>

  <!--
      mode "toc-label" matches any Hn node of a level suitable for TOC
      inclusion, and returns text content (only) with the node's own
      label (not including prefix)
  -->
  <xsl:template mode="toc-label" match="h1" name="toc-label-h1">
    <xsl:number level="any" count="h1" />
  </xsl:template>
  <xsl:template mode="toc-label" match="h2" name="toc-label-h2">
    <xsl:number level="any" count="h2" from="h1" />
  </xsl:template>
  <xsl:template mode="toc-label" match="h3" name="toc-label-h3">
    <xsl:number level="any" count="h3" from="h2" />
  </xsl:template>
  <xsl:template mode="toc-label" match="h4" name="toc-label-h4">
    <xsl:number level="any" count="h4" from="h3" />
  </xsl:template>
  <xsl:template mode="toc-label" match="h5" name="toc-label-h5">
    <xsl:number level="any" count="h5" from="h4" />
  </xsl:template>
  <xsl:template mode="toc-label" match="@*|text()" />

  <!--
      mode "toc-id" matches any Hn node of a level suitable for TOC
      inclusion, and returns text content (only) with the id tag to be
      used for the node
  -->
  <xsl:template mode="toc-id" match="h1" name="toc-id-h1">
    <xsl:text>S</xsl:text>
    <xsl:call-template name="toc-label-h1" />
  </xsl:template>
  <xsl:template mode="toc-id" match="h2" name="toc-id-h2">
    <xsl:call-template name="toc-id-h1" />
    <xsl:text>.</xsl:text>
    <xsl:call-template name="toc-label-h2" />
  </xsl:template>
  <xsl:template mode="toc-id" match="h3" name="toc-id-h3">
    <xsl:call-template name="toc-id-h2" />
    <xsl:text>.</xsl:text>
    <xsl:call-template name="toc-label-h3" />
  </xsl:template>
  <xsl:template mode="toc-id" match="h4" name="toc-id-h4">
    <xsl:call-template name="toc-id-h3" />
    <xsl:text>.</xsl:text>
    <xsl:call-template name="toc-label-h4" />
  </xsl:template>
  <xsl:template mode="toc-id" match="h5" name="toc-id-h5">
    <xsl:call-template name="toc-id-h4" />
    <xsl:text>.</xsl:text>
    <xsl:call-template name="toc-label-h5" />
  </xsl:template>
  <xsl:template mode="toc-id" match="@*|text()" />

  <!--
      mode "toc" is applied to the document's H1 elements to create
      the TOC.

      Since the document's Hn tags are unstructured and might not even
      be at top level, we have to generate the nested structure
      ourselves. Accordingly, match only on the H1 tags, and for each
      one, collect the associated H2 tags and recurse.

      Parameter "maxdepth" can be supplied by the caller.
  -->
  <xsl:template mode="toc" match="h1" name="toc-recurse">
    <xsl:param name="depth" select="2" />
    <xsl:param name="maxdepth" select="5" />
    <xsl:variable name="id">
      <xsl:apply-templates mode="toc-id" select="." />
    </xsl:variable>
    <xsl:variable name="label">
      <xsl:apply-templates mode="toc-label" select="." />
    </xsl:variable>
    <li value="{$label}" class="toc tocentry-{$depth - 1}">
      <a href="#{$id}" class="toc toclink-{$depth - 1}">
        <!-- if need be, we could make another mode for copying header
             content into TOC entries, but currently, we copy them the
             same way as in the document proper, and use CSS to tweak
             the resulting styling when needed
        -->
        <xsl:apply-templates />
      </a>
      <xsl:if test="$depth &lt;= $maxdepth and key(concat('hkey-',$depth),generate-id(.))">
        <ol class="toc toc-{$depth}">
          <xsl:for-each select="key(concat('hkey-',$depth),generate-id(.))">
            <xsl:call-template name="toc-recurse">
              <xsl:with-param name="depth" select="$depth + 1" />
              <xsl:with-param name="maxdepth" select="$maxdepth" />
            </xsl:call-template>
          </xsl:for-each>
        </ol>
      </xsl:if>
    </li>
  </xsl:template>

  <!--
      In addition to disabling normal copying of text in toc mode,
      also shortcut some common elements that can't reasonably contain
      h* nodes.
  -->
  <xsl:template mode="toc" match="p|pre|ul|dl|ol|table|@*|text()" />

  <!--
      When copying h* nodes in normal mode, fill in the id tags.

      Also interpose an otherwise inert <span> between the h* and its
      content, to avoid problems with blockification when using flex
      tricks to decorate the header.
  -->
  <xsl:template match="h1|h2|h3|h4|h5">
    <xsl:copy>
      <xsl:apply-templates select="@*" />
      <xsl:attribute name="id">
        <xsl:apply-templates mode="toc-id" select="." />
      </xsl:attribute>
      <span class="hspan-{name()}">
        <xsl:apply-templates />
      </span>
    </xsl:copy>
  </xsl:template>

  <!--
      Default actions for normal (null) mode.
  -->

  <!-- don't copy the body node, just process children. -->
  <xsl:template match="body">
    <xsl:apply-templates />
  </xsl:template>

  <!-- copy attributes on copied nodes by default too. -->
  <xsl:template match="*|@*">
    <xsl:copy>
      <xsl:apply-templates select="@*" />
      <xsl:apply-templates />
    </xsl:copy>
  </xsl:template>

  <!--
      For elements in the head, drop some attributes selectively
  -->
  <xsl:template mode="head" match="script">
    <xsl:copy>
      <!-- if type is exactly text/javascript, skip it -->
      <xsl:apply-templates select="@*[not(name()='type' and string()='text/javascript')]" />
      <xsl:apply-templates />
    </xsl:copy>
  </xsl:template>

  <!-- copy attributes on copied nodes by default too. -->
  <xsl:template mode="head" match="*|@*">
    <xsl:copy>
      <xsl:apply-templates select="@*" />
      <xsl:apply-templates />
    </xsl:copy>
  </xsl:template>

  <!--
      Actual skeleton of generated document.
  -->

  <xsl:template match="/">
    <html lang="en">
      <head>
        <title>PL/Lua Documentation</title>
        <link rel="canonical" href="{$canon_url}" />
        <meta name="viewport" content="width=device-width" />
        <xsl:apply-templates mode="head" select="//head/style[not(@id='logo.css')]" />
        <xsl:apply-templates mode="head" select="//head/script" />
        <xsl:apply-templates mode="head" select="//head/style[@id='logo.css']" />
        <xsl:apply-templates mode="head" select="//head/link[@rel='icon']" />
      </head>
      <body>
        <div id="topContainer" class="mainsection"></div>
        <div id="logoContainer" class="maincolumn">
          <div id="logo"><a href="{$canon_site}"></a></div>
        </div>
        <main id="mainContainer" class="mainsection">
          <div id="headcontent" class="maincolumn">
            <h1 id="S0">Contents</h1>
            <nav>
              <ol id="toc" class="toc toc-1">
                <xsl:apply-templates mode="toc" select="//body//h1">
                  <xsl:with-param name="maxdepth" select="3" />
                </xsl:apply-templates>
              </ol>
            </nav>
          </div>
          <div id="bodycontent" class="maincolumn bodycontent">
            <xsl:apply-templates select="//body" />
          </div>
        </main>
        <footer id="footerContainer" class="mainsection">
          <xsl:apply-templates select="//body/div[@id='footer']">
            <xsl:with-param name="footer" select="true()" />
          </xsl:apply-templates>
        </footer>
      </body>
    </html>
  </xsl:template>

</xsl:stylesheet>
