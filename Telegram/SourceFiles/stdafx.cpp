/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2009-2017 Various Qt Authors
*/
#include "stdafx.h"

/* This file contains some code from Qt source to ensure successful build.
 * TODO: find a dynamic library with these symbols.
 */

/* Debian maintainer: this function is taken from qfiledialog.cpp */
/*
    Makes a list of filters from ;;-separated text.
    Used by the mac and windows implementations
*/
QStringList qt_make_filter_list(const QString &filter)
{
    QString f(filter);

    if (f.isEmpty())
        return QStringList();

    QString sep(QLatin1String(";;"));
    int i = f.indexOf(sep, 0);
    if (i == -1) {
        if (f.indexOf(QLatin1Char('\n'), 0) != -1) {
            sep = QLatin1Char('\n');
            i = f.indexOf(sep, 0);
        }
    }

    return f.split(sep);
}

/* Debian maintainer: this constructor is taken from qtextengine.cpp for TextPainter::drawLine */
QTextItemInt::QTextItemInt(const QGlyphLayout &g, QFont *font, const QChar *chars_, int numChars, QFontEngine *fe, const QTextCharFormat &format)
    : flags(0), justified(false), underlineStyle(QTextCharFormat::NoUnderline), charFormat(format),
      num_chars(numChars), chars(chars_), logClusters(0), f(font),  glyphs(g), fontEngine(fe)
{
}

/* Debian maintainer: this method is also taken from qtextengine.cpp */
// Fix up flags and underlineStyle with given info
void QTextItemInt::initWithScriptItem(const QScriptItem &si)
{
    // explicitly initialize flags so that initFontAttributes can be called
    // multiple times on the same TextItem
    flags = 0;
    if (si.analysis.bidiLevel %2)
        flags |= QTextItem::RightToLeft;
    ascent = si.ascent;
    descent = si.descent;

    if (charFormat.hasProperty(QTextFormat::TextUnderlineStyle)) {
        underlineStyle = charFormat.underlineStyle();
    } else if (charFormat.boolProperty(QTextFormat::FontUnderline)
               || f->d->underline) {
        underlineStyle = QTextCharFormat::SingleUnderline;
    }

    // compat
    if (underlineStyle == QTextCharFormat::SingleUnderline)
        flags |= QTextItem::Underline;

    if (f->d->overline || charFormat.fontOverline())
        flags |= QTextItem::Overline;
    if (f->d->strikeOut || charFormat.fontStrikeOut())
        flags |= QTextItem::StrikeOut;
}
