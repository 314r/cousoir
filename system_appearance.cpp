#include "system_appearance.h"

#include <QEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QScreen>
#include <QStyleHints>

namespace {

QColor blended(const QColor &first, const QColor &second, qreal secondWeight)
{
    const qreal firstWeight = 1.0 - secondWeight;
    return QColor::fromRgbF(
        first.redF() * firstWeight + second.redF() * secondWeight,
        first.greenF() * firstWeight + second.greenF() * secondWeight,
        first.blueF() * firstWeight + second.blueF() * secondWeight,
        first.alphaF() * firstWeight + second.alphaF() * secondWeight);
}

QColor paletteColor(const QPalette &palette, QPalette::ColorRole role)
{
    return palette.color(QPalette::Active, role);
}

bool paletteLooksDark(const QPalette &palette)
{
    return paletteColor(palette, QPalette::Window).lightnessF() < 0.5;
}

} // namespace

SystemAppearance::SystemAppearance(QObject *parent)
    : QObject(parent)
{
    connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, &SystemAppearance::reload);
    connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, this, &SystemAppearance::reload);
    qGuiApp->installEventFilter(this);
    reload();
}

QVariantMap SystemAppearance::palette() const
{
    return m_palette;
}

QFont SystemAppearance::uiFont() const
{
    return m_uiFont;
}

QFont SystemAppearance::fixedFont() const
{
    return m_fixedFont;
}

qreal SystemAppearance::resolvedPointSize(const QFont &font)
{
    const qreal resolved = QFontInfo(font).pointSizeF();
    if (resolved > 0.0)
        return resolved;
    if (font.pointSizeF() > 0.0)
        return font.pointSizeF();
    if (font.pixelSize() > 0) {
        qreal dpi = 96.0;
        if (qGuiApp && qGuiApp->primaryScreen() && qGuiApp->primaryScreen()->logicalDotsPerInchY() > 0.0)
            dpi = qGuiApp->primaryScreen()->logicalDotsPerInchY();
        return font.pixelSize() * 72.0 / dpi;
    }
    return 9.0;
}

qreal SystemAppearance::uiFontPointSize() const
{
    return resolvedPointSize(m_uiFont);
}

bool SystemAppearance::dark() const
{
    return m_dark;
}

bool SystemAppearance::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qGuiApp
        && (event->type() == QEvent::ApplicationPaletteChange
            || event->type() == QEvent::ApplicationFontChange)) {
        reload();
    }
    return QObject::eventFilter(watched, event);
}

QVariantMap SystemAppearance::paletteFrom(const QPalette &palette, bool dark)
{
    const QColor window = paletteColor(palette, QPalette::Window);
    const QColor base = paletteColor(palette, QPalette::Base);
    const QColor button = paletteColor(palette, QPalette::Button);
    const QColor text = paletteColor(palette, QPalette::Text);
    const QColor windowText = paletteColor(palette, QPalette::WindowText);
    const QColor muted = paletteColor(palette, QPalette::PlaceholderText);
    const QColor highlight = paletteColor(palette, QPalette::Highlight);
    const QColor highlightedText = paletteColor(palette, QPalette::HighlightedText);
    const QColor middle = paletteColor(palette, QPalette::Mid);

    return {
        {QStringLiteral("background"), base},
        {QStringLiteral("dark_background"), window},
        {QStringLiteral("darker_background"), blended(window, base, 0.45)},
        {QStringLiteral("lighter_background"), button},
        {QStringLiteral("foreground"), text},
        {QStringLiteral("dark_foreground"), muted.isValid() ? muted : blended(text, base, 0.55)},
        {QStringLiteral("light_foreground"), windowText},
        {QStringLiteral("bright_foreground"), text},
        {QStringLiteral("muted"), middle},
        {QStringLiteral("accent"), highlight},
        {QStringLiteral("accent_text"), highlightedText},
        {QStringLiteral("selection"), blended(base, highlight, dark ? 0.32 : 0.22)},
        {QStringLiteral("red"), QColor(dark ? QStringLiteral("#de7979") : QStringLiteral("#a24d4d"))},
        {QStringLiteral("green"), QColor(dark ? QStringLiteral("#7bbf92") : QStringLiteral("#3f7652"))},
        {QStringLiteral("yellow"), QColor(dark ? QStringLiteral("#c5a570") : QStringLiteral("#8b671f"))},
        {QStringLiteral("cyan"), QColor(dark ? QStringLiteral("#9fc7db") : QStringLiteral("#3d7089"))},
    };
}

void SystemAppearance::reload()
{
    const QPalette applicationPalette = QGuiApplication::palette();
    const Qt::ColorScheme scheme = qGuiApp->styleHints()->colorScheme();
    const bool dark = scheme == Qt::ColorScheme::Dark
        || (scheme == Qt::ColorScheme::Unknown && paletteLooksDark(applicationPalette));
    const QVariantMap palette = paletteFrom(applicationPalette, dark);
    const QFont uiFont = QGuiApplication::font();
    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    const bool paletteDidChange = m_palette != palette || m_dark != dark;
    const bool fontsDidChange = m_uiFont != uiFont || m_fixedFont != fixedFont;

    m_palette = palette;
    m_uiFont = uiFont;
    m_fixedFont = fixedFont;
    m_dark = dark;

    if (paletteDidChange)
        emit paletteChanged();
    if (fontsDidChange)
        emit fontsChanged();
}
