pragma Singleton

import QtQuick

// Omarchy design language: each mode reduces to a background, a foreground
// and one accent. Interaction states are foreground-tinted alphas shared by
// every control, never accent fills.
QtObject {
    id: theme

    property string mode: "system"
    property bool systemDark: false
    property bool omarchyDark: true
    property bool omarchyAvailable: false

    property var systemPalette: ({
        "background": "#ffffff",
        "dark_background": "#efefef",
        "foreground": "#25272a",
        "bright_foreground": "#25272a",
        "accent": "#5980a6",
        "red": "#a24d4d"
    })
    property string systemMonoFont: "monospace"
    property real systemFontPointSize: 9
    property int textScalePercent: 100

    property var omarchyPalette: ({
        "background": "#030203",
        "dark_background": "#080708",
        "darker_background": "#050505",
        "foreground": "#b5bfc4",
        "bright_foreground": "#b3bbbf",
        "accent": "#b85d4d",
        "red": "#b85d4d"
    })

    readonly property var lightBase: ({
        "background": "#f2f2f3",
        "foreground": "#25272a",
        "bright": "#0f1012",
        "accent": "#5980a6",
        "danger": "#a24d4d"
    })
    readonly property var darkBase: ({
        "background": "#141414",
        "foreground": "#e6e6e6",
        "bright": "#ffffff",
        "accent": "#6da3d8",
        "danger": "#de7979"
    })

    readonly property string effectiveMode: mode === "omarchy" && !omarchyAvailable ? "system" : mode
    readonly property bool systemMode: effectiveMode === "system"
    readonly property bool omarchyMode: effectiveMode === "omarchy"
    readonly property bool darkMode: effectiveMode === "dark"
        || (systemMode && systemDark)
        || (omarchyMode && omarchyDark)

    function paletteBase(palette) {
        return {
            "background": palette.background,
            "foreground": palette.foreground,
            "bright": palette.bright_foreground || palette.foreground,
            "accent": palette.accent,
            "danger": palette.red
        }
    }

    readonly property var base: systemMode ? paletteBase(systemPalette)
        : omarchyMode ? paletteBase(omarchyPalette)
        : darkMode ? darkBase : lightBase

    function withAlpha(value: color, alpha: real): color {
        return Qt.rgba(value.r, value.g, value.b, alpha)
    }

    function mix(from: color, to: color, ratio: real): color {
        return Qt.rgba(from.r + (to.r - from.r) * ratio,
                       from.g + (to.g - from.g) * ratio,
                       from.b + (to.b - from.b) * ratio, 1)
    }

    readonly property color canvas: base.background
    readonly property color text: base.foreground
    // Some palettes ship a bright_foreground dimmer than the foreground.
    readonly property color paletteBright: base.bright
    readonly property color textBright: Math.abs(paletteBright.hslLightness - canvas.hslLightness)
        > Math.abs(text.hslLightness - canvas.hslLightness) ? paletteBright : text
    readonly property color textMuted: mix(canvas, text, 0.75)
    readonly property color textFaint: mix(canvas, text, 0.5)
    readonly property color accent: base.accent
    readonly property color danger: base.danger
    readonly property color fill: withAlpha(text, 0.04)
    readonly property color hover: withAlpha(text, 0.08)
    readonly property color selected: withAlpha(text, 0.18)
    readonly property color pressed: withAlpha(text, 0.22)
    readonly property color divider: withAlpha(text, 0.12)
    readonly property color controlBorder: withAlpha(text, 0.4)
    readonly property color textSelection: withAlpha(text, 0.35)

    readonly property string uiFont: systemMonoFont
    readonly property int effectiveTextScalePercent: Math.max(80, Math.min(200, textScalePercent))
    readonly property real effectiveFontPointSize: Math.max(1, systemFontPointSize)
        * effectiveTextScalePercent / 100
    // 12/11/10/14/16 px at the default size: body, secondary text, uppercase
    // headings, titles and the largest numbers.
    readonly property real fontPointSizeBody: effectiveFontPointSize
    readonly property real fontPointSizeCaption: effectiveFontPointSize * 11 / 12
    readonly property real fontPointSizeLabel: effectiveFontPointSize * 10 / 12
    readonly property real fontPointSizeTitle: effectiveFontPointSize * 14 / 12
    readonly property real fontPointSizeDisplay: effectiveFontPointSize * 16 / 12

    // Rows, fields and buttons share one height: a line plus 6 px above and
    // below. Spacing follows the user's text size, not the font.
    readonly property real spacingScale: effectiveTextScalePercent / 100
    readonly property int rowHeight: Math.max(24, Math.round(bodyMetrics.height + 12 * spacingScale))
    readonly property int toolbarHeight: rowHeight + Math.round(12 * spacingScale)
    readonly property int statusHeight: Math.max(22, Math.round(captionMetrics.height + 10 * spacingScale))
    readonly property int iconSize: Math.max(12, Math.round(bodyMetrics.height * 0.875))
    // A choice's chevron and the status bar's icons.
    readonly property int smallIconSize: Math.round(iconSize * 0.85)
    // Between the edge of a field, a choice, a menu or a text button and its
    // text.
    readonly property int controlInset: Math.round(10 * spacingScale)
    readonly property int gutter: Math.round(8 * spacingScale)
    readonly property int rowPadding: Math.round(12 * spacingScale)
    readonly property int panePadding: Math.round(18 * spacingScale)
    readonly property int pagePadding: Math.round(24 * spacingScale)
    readonly property int columnGap: Math.round(16 * spacingScale)
    readonly property int paneWidth: Math.round(Math.min(320 * spacingScale, 440))
    // Widest of the average and a bold "M": exact for a monospace font, and
    // never narrower than bold column headers.
    readonly property real charWidth: Math.max(5, bodyMetrics.averageCharacterWidth,
        bodyMetrics.advanceWidth("M"), bodyStrongMetrics.advanceWidth("M"))
    readonly property real captionCharWidth: Math.max(5, captionMetrics.averageCharacterWidth,
        captionMetrics.advanceWidth("M"), captionStrongMetrics.advanceWidth("M"))

    readonly property FontMetrics bodyMetrics: FontMetrics {
        font.family: theme.uiFont
        font.pointSize: theme.fontPointSizeBody
    }

    readonly property FontMetrics captionMetrics: FontMetrics {
        font.family: theme.uiFont
        font.pointSize: theme.fontPointSizeCaption
    }

    readonly property FontMetrics bodyStrongMetrics: FontMetrics {
        font.family: theme.uiFont
        font.pointSize: theme.fontPointSizeBody
        font.weight: Font.Bold
    }

    readonly property FontMetrics captionStrongMetrics: FontMetrics {
        font.family: theme.uiFont
        font.pointSize: theme.fontPointSizeCaption
        font.weight: Font.Bold
    }
}
