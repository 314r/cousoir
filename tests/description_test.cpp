#include <QtTest>

#include "description.h"

class DescriptionTest : public QObject
{
    Q_OBJECT

private slots:
    void readsHtml_data();
    void readsHtml();
    void writesHtml_data();
    void writesHtml();
    void keepsItsText_data();
    void keepsItsText();
    void tellsWhatIsLost();
    void cleansForShowing();
};

void DescriptionTest::readsHtml_data()
{
    QTest::addColumn<QString>("html");
    QTest::addColumn<QString>("text");
    QTest::newRow("paragraphs and breaks")
        << QStringLiteral("<div><p>Un mystère gît…<br>Pour les uns.</p><p>Né au Québec.</p></div>")
        << QStringLiteral("Un mystère gît…\nPour les uns.\n\nNé au Québec.");
    QTest::newRow("bold, italics, links")
        << QStringLiteral("<p>Les <b>hauts-fonds</b> et <i>Vietnamiens</i>, <a href=\"https://x.org/a b\">Ray</a>.</p>")
        << QStringLiteral("Les **hauts-fonds** et *Vietnamiens*, [Ray](https://x.org/a%20b).");
    QTest::newRow("both") << QStringLiteral("<p><b><i>Tous deux</i></b></p>") << QStringLiteral("**_Tous deux_**");
    QTest::newRow("lists")
        << QStringLiteral("<ul><li>un</li><li>deux</li></ul><ol><li>premier</li><li>second</li></ol>")
        << QStringLiteral("- un\n- deux\n\n1. premier\n2. second");
    QTest::newRow("heading") << QStringLiteral("<h3>Titre</h3><p>Texte</p>") << QStringLiteral("### Titre\n\nTexte");
    QTest::newRow("marks in the text are plain")
        << QStringLiteral("<p>5 * 3 = [quinze] _x_ \\</p><p># 1</p><p>- non</p><p>1. non plus</p>")
        << QStringLiteral("5 \\* 3 = \\[quinze\\] \\_x\\_ \\\\\n\n\\# 1\n\n\\- non\n\n1\\. non plus");
    QTest::newRow("plain text, as Calibre stores some")
        << QStringLiteral("Première ligne\nseconde\n\nAutre paragraphe")
        << QStringLiteral("Première ligne\nseconde\n\nAutre paragraphe");
    QTest::newRow("fonts and colours go")
        << QStringLiteral("<p style=\"color:red\"><font face=\"Arial\">rouge</font> <span>simple</span></p>")
        << QStringLiteral("rouge simple");
    QTest::newRow("empty") << QString() << QString();
}

void DescriptionTest::readsHtml()
{
    QFETCH(QString, html);
    QFETCH(QString, text);
    QCOMPARE(Description::toText(html), text);
}

void DescriptionTest::writesHtml_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("html");
    QTest::newRow("paragraphs and breaks") << QStringLiteral("Un\ndeux\n\n\nTrois")
                                           << QStringLiteral("<div><p>Un<br>deux</p><p>Trois</p></div>");
    QTest::newRow("marks") << QStringLiteral("**gras** *italique* _aussi_ __gras__ snake_case_name [lien](https://a.b?c=d&e=f)")
                           << QStringLiteral("<div><p><strong>gras</strong> <em>italique</em> <em>aussi</em> "
                                             "<strong>gras</strong> snake_case_name "
                                             "<a href=\"https://a.b?c=d&amp;e=f\">lien</a></p></div>");
    QTest::newRow("html is text") << QStringLiteral("<b>non</b> & \"oui\"")
                                  << QStringLiteral("<div><p>&lt;b&gt;non&lt;/b&gt; &amp; &quot;oui&quot;</p></div>");
    QTest::newRow("escapes") << QStringLiteral("5 \\* 3 \\[x\\] \\_y\\_ \\\\")
                             << QStringLiteral("<div><p>5 * 3 [x] _y_ \\</p></div>");
    QTest::newRow("lists") << QStringLiteral("- un\n  suite\n- deux\n\n3. trois\n4) quatre")
                           << QStringLiteral("<div><ul><li>un<br>suite</li><li>deux</li></ul>"
                                             "<ol start=\"3\"><li>trois</li><li>quatre</li></ol></div>");
    QTest::newRow("heading") << QStringLiteral("## Titre ##\nTexte")
                             << QStringLiteral("<div><h2>Titre</h2><p>Texte</p></div>");
    QTest::newRow("blank") << QStringLiteral(" \n\n ") << QString();
}

void DescriptionTest::writesHtml()
{
    QFETCH(QString, text);
    QFETCH(QString, html);
    QCOMPARE(Description::toHtml(text), html);
}

// A description edited once and saved reads back as the same text.
void DescriptionTest::keepsItsText_data()
{
    QTest::addColumn<QString>("html");
    QTest::newRow("Calibre's") << QStringLiteral(
        "<div><p>Un mystère gît dans les hauts-fonds de l'archipel de Côn Dao…<br>Pour les Vietnamiens des "
        "environs, ce sont des monstres assassins.</p><p>« Un thriller spéculatif de premier ordre » <b>JEFF "
        "VANDERMEER</b></p></div>");
    QTest::newRow("publisher's") << QStringLiteral(
        "<h3>Résumé</h3><p>Un <em>roman</em> en <strong>trois</strong> parties&nbsp;: <a href=\"http://x\">site</a>"
        "</p><ul><li>prix *Hugo* 1975</li><li>prix [Nebula]</li></ul><p>1. Pas une liste</p>");
    QTest::newRow("plain") << QStringLiteral("Ligne\nautre\n\n- pas une liste ?\n# ni un titre");
}

void DescriptionTest::keepsItsText()
{
    QFETCH(QString, html);
    const QString text = Description::toText(html);
    QVERIFY(!text.isEmpty());
    QCOMPARE(Description::toText(Description::toHtml(text)), text);
}

void DescriptionTest::tellsWhatIsLost()
{
    QVERIFY(!Description::losesFormatting(QStringLiteral("<div><p>a <b>b</b> <span style=\"x\">c</span></p></div>")));
    QVERIFY(!Description::losesFormatting(QStringLiteral("plain text < 5")));
    QVERIFY(Description::losesFormatting(QStringLiteral("<table><tr><td>a</td></tr></table>")));
    QVERIFY(Description::losesFormatting(QStringLiteral("<p><img src=\"x.png\"></p>")));
    QVERIFY(Description::losesFormatting(QStringLiteral("<blockquote>a</blockquote>")));
}

void DescriptionTest::cleansForShowing()
{
    QCOMPARE(Description::cleaned(QStringLiteral("a\nb\n\nc & d")),
             QStringLiteral("<p>a<br>b</p><p>c &amp; d</p>"));
    QCOMPARE(Description::cleaned(QStringLiteral("<p style=\"color:red\"><font size=3>x</font></p>")),
             QStringLiteral("<p>x</p>"));
}

QTEST_MAIN(DescriptionTest)
#include "description_test.moc"
