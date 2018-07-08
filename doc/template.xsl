<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:template mode="dl-br-dt" match="*">
    <dt>
      <xsl:copy>
        <xsl:apply-templates select="@*|node()" />
      </xsl:copy>
    </dt>
  </xsl:template>
  <xsl:template mode="dl-br-dt" match="br">
  </xsl:template>
  <xsl:template mode="dl-br" match="li[child::br]">
    <xsl:apply-templates mode="dl-br-dt" select="node()[following-sibling::br]" />
    <xsl:if test="child::br[last() and (following-sibling::* or normalize-space(following-sibling::text()))]">
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
      <xsl:apply-templates select="@*|node()" />
    </xsl:copy>
  </xsl:template>
  <xsl:template match="/">
    <html>
      <head>
        <title>Title</title>
      </head>
      <xsl:apply-templates />
    </html>
  </xsl:template>
</xsl:stylesheet>
