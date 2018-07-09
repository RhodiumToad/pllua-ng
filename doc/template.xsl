<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">

  <!--<xsl:key name="h1" match="h1" use="1 + count(preceding-sibling:h1)" />-->

  <!--
      Break up a list of nodes at each top-level <br>, and turn each
      sublist into a <dt>...</dt> node. Initially nfirst is empty and
      nlist has the remaining nodes; if nlist doesn't start with a
      <br> node, then shift one node from nlist to nfirst and recurse;
      nlist is empty or starts with <br>, then output nfirst, empty
      it, and recurse with the remainder of nlist. (We also eat a
      whitespace-only text node after the <br> for purely aesthetic
      reasons.)
  -->
  <xsl:template name="dl-br-dt">
    <xsl:param name="nfirst" />
    <xsl:param name="nlist" />
    <xsl:choose>
      <xsl:when test="not($nfirst or $nlist)" />
      <xsl:when test="$nlist[1][self::br] or not($nlist)">
        <dt>
          <xsl:apply-templates select="$nfirst" />
        </dt>
        <xsl:call-template name="dl-br-dt">
          <xsl:with-param name="nfirst" select="$nfirst[false()]" />
          <xsl:with-param name="nlist" select="$nlist[position() &gt; 1 and (position() &gt; 2 or * or normalize-space())]" />
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="dl-br-dt">
          <xsl:with-param name="nfirst" select="$nfirst | $nlist[1]" />
          <xsl:with-param name="nlist" select="$nlist[position() &gt; 1]" />
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template mode="dl-br" match="li[child::br]">
    <xsl:call-template name="dl-br-dt">
      <xsl:with-param name="nfirst" select="*[false()]" />
      <xsl:with-param name="nlist" select="node()[following-sibling::br]" />
    </xsl:call-template>
    <xsl:if test="child::br[position()=last() and (following-sibling::* or normalize-space(following-sibling::text()))]">
      <dd>
        <xsl:apply-templates select="node()[not(following-sibling::br or self::br)]" />
      </dd>
    </xsl:if>
  </xsl:template>

  <xsl:template mode="dl-p" match="li[child::p]">
    <dt>
      <xsl:apply-templates select="p[1]/node()" />
    </dt>
    <xsl:if test="child::p[following-sibling::p]">
      <dd>
        <xsl:apply-templates select="p[position() != 1]|node()[not(self::p)]" />
      </dd>
    </xsl:if>
  </xsl:template>

  <xsl:template mode="dl-br" match="*|@*">
    <xsl:copy>
      <xsl:apply-templates select="@*|node()" />
    </xsl:copy>
  </xsl:template>
  <xsl:template mode="dl-p" match="*|@*">
    <xsl:copy>
      <xsl:apply-templates select="@*|node()" />
    </xsl:copy>
  </xsl:template>

  <xsl:template match="ul[child::li[1][child::br] and not(parent::div[@class='no-dl-fudge'])]">
    <dl>
      <xsl:apply-templates mode="dl-br" select="node()" />
    </dl>
  </xsl:template>

  <xsl:template match="ul[child::li[1][child::p] and not(parent::div[@class='no-dl-fudge'])]">
    <dl>
      <xsl:apply-templates mode="dl-p" select="node()" />
    </dl>
  </xsl:template>

  <xsl:template match="*|@*">
    <xsl:copy>
      <xsl:apply-templates select="node()" />
    </xsl:copy>
  </xsl:template>

  <xsl:template match="code[not(ancestor::pre)]">
    <xsl:copy>
      <xsl:if test="string-length() &lt; 40">
        <xsl:attribute name="class">shortcode</xsl:attribute>
      </xsl:if>
      <xsl:apply-templates select="node()" />
    </xsl:copy>
  </xsl:template>

  <xsl:template match="h1">
    <xsl:variable name="n" select="1 + count(preceding-sibling::h1)" />
    <xsl:copy>
      <xsl:attribute name="id"><xsl:value-of select="concat('S',$n)" /></xsl:attribute>
      <xsl:apply-templates select="@*|node()" />
    </xsl:copy>
  </xsl:template>

  <xsl:template match="h2">
    <xsl:variable name="n1" select="count(preceding-sibling::h1)" />
    <xsl:variable name="n2" select="1 + count(preceding-sibling::h2) - count(preceding-sibling::h1[1]/preceding-sibling::h2)" />
    <xsl:copy>
      <xsl:attribute name="id"><xsl:value-of select="concat('S',$n1,'.',$n2)" /></xsl:attribute>
      <xsl:apply-templates select="@*|node()" />
    </xsl:copy>
  </xsl:template>

  <xsl:template name="toc2">
    <xsl:variable name="n1" select="count(preceding-sibling::h1)" />
    <xsl:variable name="n2" select="1 + count(preceding-sibling::h2) - count(preceding-sibling::h1[1]/preceding-sibling::h2)" />
    <li value="{$n2}">
      <a href="#S{concat($n1,'.',$n2)}">
        <xsl:value-of select="." />
      </a>
    </li>
  </xsl:template>

  <xsl:template name="toc1">
    <xsl:variable name="n" select="1 + count(preceding-sibling::h1)" />
    <li value="{$n}">
      <a href="#S{$n}">
        <xsl:value-of select="." />
      </a>
      <xsl:if test="following-sibling::h2[count(preceding-sibling::h1) = $n]">
        <ol>
          <xsl:for-each select="following-sibling::h2[count(preceding-sibling::h1) = $n]">
            <xsl:call-template name="toc2" />
          </xsl:for-each>
        </ol>
      </xsl:if>
    </li>
  </xsl:template>

  <xsl:template match="/">
    <html>
      <head>
        <title>Title</title>
      </head>
      <body>
        <ol>
          <xsl:for-each select="//body/h1">
            <xsl:call-template name="toc1" />
          </xsl:for-each>
        </ol>
        <xsl:apply-templates select="//body/node()" />
      </body>
    </html>
  </xsl:template>

</xsl:stylesheet>
