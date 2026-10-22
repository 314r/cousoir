#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// A QR code, as ISO/IEC 18004 makes one: the text as bytes, error
// correction level M, in the smallest version that holds it, with the mask
// that scores the fewest penalty points. For the addresses a phone opens by
// pointing its camera at the screen.
namespace QrCode {

struct Matrix
{
    int version = 0;
    int mask = 0;
    // Modules per side; 0 for a text too long for any QR code.
    int size = 0;
    // Row by row, true for dark.
    QList<bool> modules;

    bool dark(int x, int y) const { return modules.at(y * size + x); }
};

// `mask` from 0 to 7 forces that mask, for the tests; -1 picks it.
Matrix encode(const QByteArray &data, int mask = -1);

// An SVG drawing of the code, a module a unit, with the quiet zone of four
// modules around it that readers need.
QByteArray svg(const Matrix &matrix);

} // namespace QrCode
