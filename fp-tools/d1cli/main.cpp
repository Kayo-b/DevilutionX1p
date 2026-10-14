// Headless driver for d1-graphics-tool's format code: load a level tileset (CEL+MIN+TIL+SOL+AMP) with a palette and
// export every subtile, every 2.5D tile, every flat tile and every CEL frame as PNGs, plus a CSV of subtile metadata.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QTextStream>
#include <cstdio>

#include "d1formats/d1amp.h"
#include "d1formats/d1celtileset.h"
#include "d1formats/d1gfx.h"
#include "d1formats/d1min.h"
#include "d1formats/d1sol.h"
#include "d1formats/d1til.h"
#include "palette/d1pal.h"

static void sheet(const QList<QImage> &ims, int perLine, const QString &path)
{
    int w = 0, h = 0;
    for (auto &im : ims) { w = std::max(w, im.width()); h = std::max(h, im.height()); }
    int rows = (ims.size() + perLine - 1) / perLine;
    QImage out(w * perLine, h * rows, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    for (int i = 0; i < ims.size(); i++) p.drawImage((i % perLine) * w, (i / perLine) * h, ims[i]);
    p.end();
    out.save(path);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc < 4) { fprintf(stderr, "usage: d1cli <level.cel> <palette.pal> <outdir>\n"); return 2; }
    QString cel = argv[1], palPath = argv[2], out = argv[3];
    QDir().mkpath(out + "/subtiles"); QDir().mkpath(out + "/tiles"); QDir().mkpath(out + "/frames");
    QString base = cel.left(cel.lastIndexOf('.'));
    OpenAsParam params; params.celFilePath = cel; params.isTileset = OPEN_TILESET_TYPE::Yes;
    D1Pal pal; if (!pal.load(palPath)) { fprintf(stderr, "palette\n"); return 1; }
    D1Gfx gfx; gfx.setPalette(&pal);
    D1Sol sol; if (!sol.load(base + ".sol")) { fprintf(stderr, "sol\n"); return 1; }
    D1Min min; std::map<unsigned, D1CEL_FRAME_TYPE> types;
    if (!min.load(base + ".min", &gfx, &sol, types, params)) { fprintf(stderr, "min\n"); return 1; }
    D1Til til; if (!til.load(base + ".til", &min)) { fprintf(stderr, "til\n"); return 1; }
    D1Amp amp; amp.load(base + ".amp", til.getTileCount(), params);
    if (!D1CelTileset::load(gfx, types, cel, params)) { fprintf(stderr, "cel\n"); return 1; }

    QList<QImage> subs, tiles;
    QFile csv(out + "/subtiles.csv"); csv.open(QIODevice::WriteOnly); QTextStream t(&csv);
    t << "subtile,sol,frames\n";
    for (int i = 0; i < min.getSubtileCount(); i++) {
        QImage im = min.getSubtileImage(i); subs << im;
        im.save(QString("%1/subtiles/%2.png").arg(out).arg(i + 1, 4, 10, QChar('0')));
        QStringList fr; for (auto f : min.getCelFrameIndices(i)) fr << QString::number(f);
        t << (i + 1) << "," << sol.getSubtileProperties(i) << "," << fr.join(' ') << "\n";
    }
    QFile tcsv(out + "/tiles.csv"); tcsv.open(QIODevice::WriteOnly); QTextStream tt(&tcsv);
    tt << "tile,amp_type,amp_flags,subtiles\n";
    for (int i = 0; i < til.getTileCount(); i++) {
        QImage im = til.getTileImage(i); tiles << im;
        im.save(QString("%1/tiles/%2.png").arg(out).arg(i + 1, 4, 10, QChar('0')));
        QStringList st; for (auto s : til.getSubtileIndices(i)) st << QString::number(s + 1);
        tt << (i + 1) << "," << amp.getTileType(i) << "," << amp.getTileProperties(i) << "," << st.join(' ') << "\n";
    }
    for (int i = 0; i < gfx.getFrameCount(); i++)
        gfx.getFrameImage(i).save(QString("%1/frames/%2.png").arg(out).arg(i + 1, 4, 10, QChar('0')));
    sheet(subs, 20, out + "/subtiles_sheet.png");
    sheet(tiles, 10, out + "/tiles_sheet.png");
    printf("subtiles %d (%dx%d), tiles %d, frames %d\n", min.getSubtileCount(), min.getSubtileWidth(), min.getSubtileHeight(), til.getTileCount(), gfx.getFrameCount());
    return 0;
}
