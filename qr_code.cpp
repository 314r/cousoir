#include "qr_code.h"

// After the structure of Project Nayuki's QR Code generator library,
// Copyright (c) Project Nayuki, MIT license (see CREDITS.md): the
// Reed-Solomon arithmetic, the blocks and their interleaving, the patterns,
// the codewords' path and the masks.

#include <algorithm>
#include <array>
#include <cstdlib>

namespace QrCode {

namespace {

// Error correction level M, by version from 1 to 40: the error correction
// codewords of each block, and the blocks (ISO/IEC 18004, table 9).
constexpr std::array<int, 41> eccPerBlock{ 0,  10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22,
                                           24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28,
                                           28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28 };
constexpr std::array<int, 41> blockCount{ 0,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,
                                          9,  10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25,
                                          26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49 };
// Level M's two bits in the format information.
constexpr int levelBits = 0;

// The modules that carry data and error correction, once the patterns are
// drawn.
int rawDataModules(int version)
{
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int alignments = version / 7 + 2;
        result -= (25 * alignments - 10) * alignments - 55;
        if (version >= 7)
            result -= 36;
    }
    return result;
}

int dataCodewords(int version)
{
    return rawDataModules(version) / 8 - eccPerBlock[version] * blockCount[version];
}

// Multiplication in GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1.
quint8 multiply(quint8 x, quint8 y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11d);
        z ^= ((y >> i) & 1) * x;
    }
    return quint8(z);
}

// The Reed-Solomon generator of `degree`, without its leading 1: the
// product of (x - 2^i) for i below `degree`.
QByteArray generator(int degree)
{
    QByteArray result(degree, 0);
    result[degree - 1] = 1;
    quint8 root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            result[j] = char(multiply(quint8(result.at(j)), root));
            if (j + 1 < degree)
                result[j] = char(quint8(result.at(j)) ^ quint8(result.at(j + 1)));
        }
        root = multiply(root, 0x02);
    }
    return result;
}

QByteArray remainder(const QByteArray &data, const QByteArray &divisor)
{
    QByteArray result(divisor.size(), 0);
    for (const char byte : data) {
        const quint8 factor = quint8(byte) ^ quint8(result.at(0));
        result.remove(0, 1);
        result.append(char(0));
        for (qsizetype i = 0; i < result.size(); ++i)
            result[i] = char(quint8(result.at(i)) ^ multiply(quint8(divisor.at(i)), factor));
    }
    return result;
}

class Builder
{
public:
    explicit Builder(int version) : version(version), size(version * 4 + 17)
    {
        modules.fill(false, size * size);
        function.fill(false, size * size);
    }

    void set(int x, int y, bool dark)
    {
        modules[y * size + x] = dark;
        function[y * size + x] = true;
    }

    void drawPatterns()
    {
        for (int i = 0; i < size; ++i) {
            set(6, i, i % 2 == 0);
            set(i, 6, i % 2 == 0);
        }
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);
        const QList<int> positions = alignmentPositions();
        const qsizetype count = positions.size();
        for (qsizetype i = 0; i < count; ++i) {
            for (qsizetype j = 0; j < count; ++j) {
                // Not over the finders.
                if ((i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0))
                    continue;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx)
                        set(positions.at(i) + dx, positions.at(j) + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
                }
            }
        }
        // Reserved for now, drawn for the mask chosen.
        drawFormat(0);
        drawVersion();
    }

    void drawFinder(int x, int y)
    {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int distance = std::max(std::abs(dx), std::abs(dy));
                if (x + dx >= 0 && x + dx < size && y + dy >= 0 && y + dy < size)
                    set(x + dx, y + dy, distance != 2 && distance != 4);
            }
        }
    }

    QList<int> alignmentPositions() const
    {
        if (version == 1)
            return {};
        const int count = version / 7 + 2;
        const int step = (version * 8 + count * 3 + 5) / (count * 4 - 4) * 2;
        QList<int> result;
        for (int i = 0, position = size - 7; i < count - 1; ++i, position -= step)
            result.prepend(position);
        result.prepend(6);
        return result;
    }

    void drawFormat(int mask)
    {
        const int data = levelBits << 3 | mask;
        int bits = data;
        for (int i = 0; i < 10; ++i)
            bits = (bits << 1) ^ ((bits >> 9) * 0x537);
        bits = (data << 10 | bits) ^ 0x5412;
        const auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i)
            set(8, i, bit(i));
        set(8, 7, bit(6));
        set(8, 8, bit(7));
        set(7, 8, bit(8));
        for (int i = 9; i < 15; ++i)
            set(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i)
            set(size - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i)
            set(8, size - 15 + i, bit(i));
        set(8, size - 8, true);
    }

    void drawVersion()
    {
        if (version < 7)
            return;
        int bits = version;
        for (int i = 0; i < 12; ++i)
            bits = (bits << 1) ^ ((bits >> 11) * 0x1f25);
        bits = version << 12 | bits;
        for (int i = 0; i < 18; ++i) {
            const bool dark = ((bits >> i) & 1) != 0;
            const int a = size - 11 + i % 3;
            const int b = i / 3;
            set(a, b, dark);
            set(b, a, dark);
        }
    }

    // The codewords in their zigzag, two columns at a time from the right.
    void drawCodewords(const QByteArray &codewords)
    {
        qsizetype i = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5;
            for (int vertical = 0; vertical < size; ++vertical) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size - 1 - vertical : vertical;
                    if (!function.at(y * size + x) && i < codewords.size() * 8) {
                        modules[y * size + x] = ((quint8(codewords.at(i >> 3)) >> (7 - (i & 7))) & 1) != 0;
                        ++i;
                    }
                }
            }
        }
    }

    void applyMask(int mask)
    {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                bool invert = false;
                switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (invert && !function.at(y * size + x))
                    modules[y * size + x] = !modules.at(y * size + x);
            }
        }
    }

    bool dark(int x, int y) const { return modules.at(y * size + x); }

    // The penalty points of the four rules of ISO/IEC 18004, 7.8.3.
    long penalty() const
    {
        long result = 0;
        // Rows then columns: runs of five or more alike, and 1:1:3:1:1
        // patterns with four light modules on a side.
        for (int pass = 0; pass < 2; ++pass) {
            for (int a = 0; a < size; ++a) {
                const auto at = [&](int b) { return pass == 0 ? dark(b, a) : dark(a, b); };
                int run = 0;
                bool color = false;
                for (int b = 0; b < size; ++b) {
                    if (b > 0 && at(b) == color) {
                        ++run;
                    } else {
                        if (run >= 5)
                            result += 3 + (run - 5);
                        color = at(b);
                        run = 1;
                    }
                }
                if (run >= 5)
                    result += 3 + (run - 5);
                for (int b = 0; b + 7 <= size; ++b) {
                    if (!(at(b) && !at(b + 1) && at(b + 2) && at(b + 3) && at(b + 4) && !at(b + 5) && at(b + 6)))
                        continue;
                    const auto light = [&](int from, int to) {
                        for (int c = from; c < to; ++c) {
                            if (c >= 0 && c < size && at(c))
                                return false;
                        }
                        return true;
                    };
                    if (light(b - 4, b) || light(b + 7, b + 11))
                        result += 40;
                }
            }
        }
        for (int y = 0; y + 1 < size; ++y) {
            for (int x = 0; x + 1 < size; ++x) {
                const bool color = dark(x, y);
                if (color == dark(x + 1, y) && color == dark(x, y + 1) && color == dark(x + 1, y + 1))
                    result += 3;
            }
        }
        const long total = long(size) * size;
        const long darkCount = std::count(modules.cbegin(), modules.cend(), true);
        // The fewest steps of 5 % from half dark.
        const long k = (std::abs(darkCount * 20 - total * 10) + total - 1) / total - 1;
        result += k * 10;
        return result;
    }

    int version;
    int size;
    QList<bool> modules;
    QList<bool> function;
};

} // namespace

Matrix encode(const QByteArray &data, int mask)
{
    int version = 1;
    for (; version <= 40; ++version) {
        const int countBits = version <= 9 ? 8 : 16;
        if (data.size() < (1 << countBits) && 4 + countBits + data.size() * 8 <= dataCodewords(version) * 8)
            break;
    }
    if (version > 40)
        return {};

    // The bits: byte mode, the count, the bytes, a terminator, then padding.
    QList<bool> bits;
    const auto append = [&bits](quint32 value, int length) {
        for (int i = length - 1; i >= 0; --i)
            bits.append(((value >> i) & 1) != 0);
    };
    append(0x4, 4);
    append(quint32(data.size()), version <= 9 ? 8 : 16);
    for (const char byte : data)
        append(quint8(byte), 8);
    const int capacity = dataCodewords(version) * 8;
    append(0, std::min(4, int(capacity - bits.size())));
    append(0, int((8 - bits.size() % 8) % 8));
    QByteArray codewords;
    for (qsizetype i = 0; i < bits.size(); i += 8) {
        quint8 byte = 0;
        for (int j = 0; j < 8; ++j)
            byte = quint8(byte << 1 | (bits.at(i + j) ? 1 : 0));
        codewords.append(char(byte));
    }
    for (quint8 pad = 0xec; codewords.size() < capacity / 8; pad ^= 0xec ^ 0x11)
        codewords.append(char(pad));

    // Split into blocks, each with its error correction, then interleaved.
    const int blocks = blockCount[version];
    const int eccLength = eccPerBlock[version];
    const int rawCodewords = rawDataModules(version) / 8;
    const int shortBlocks = blocks - rawCodewords % blocks;
    const int shortLength = rawCodewords / blocks;
    const QByteArray divisor = generator(eccLength);
    QList<QByteArray> split;
    for (int i = 0, at = 0; i < blocks; ++i) {
        const int length = shortLength - eccLength + (i < shortBlocks ? 0 : 1);
        QByteArray block = codewords.mid(at, length);
        at += length;
        const QByteArray ecc = remainder(block, divisor);
        // A short block takes a placeholder, left out below, to line up.
        if (i < shortBlocks)
            block.append(char(0));
        split.append(block + ecc);
    }
    QByteArray interleaved;
    for (int i = 0; i < split.first().size(); ++i) {
        for (int j = 0; j < blocks; ++j) {
            if (i != shortLength - eccLength || j >= shortBlocks)
                interleaved.append(split.at(j).at(i));
        }
    }

    Builder builder(version);
    builder.drawPatterns();
    builder.drawCodewords(interleaved);
    if (mask < 0 || mask > 7) {
        long lowest = -1;
        for (int candidate = 0; candidate < 8; ++candidate) {
            builder.applyMask(candidate);
            builder.drawFormat(candidate);
            const long points = builder.penalty();
            if (lowest < 0 || points < lowest) {
                lowest = points;
                mask = candidate;
            }
            // A mask undoes itself.
            builder.applyMask(candidate);
        }
    }
    builder.applyMask(mask);
    builder.drawFormat(mask);

    Matrix matrix;
    matrix.version = version;
    matrix.mask = mask;
    matrix.size = builder.size;
    matrix.modules = builder.modules;
    return matrix;
}

QByteArray svg(const Matrix &matrix)
{
    constexpr int border = 4;
    const int side = matrix.size + 2 * border;
    QByteArray path;
    for (int y = 0; y < matrix.size; ++y) {
        for (int x = 0; x < matrix.size; ++x) {
            if (matrix.dark(x, y))
                path += 'M' + QByteArray::number(x + border) + ',' + QByteArray::number(y + border) + "h1v1h-1z";
        }
    }
    const QByteArray viewBox = "0 0 " + QByteArray::number(side) + ' ' + QByteArray::number(side);
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"" + viewBox + "\" shape-rendering=\"crispEdges\">"
        + "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/><path fill=\"#000\" d=\"" + path + "\"/></svg>";
}

} // namespace QrCode
