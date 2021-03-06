/*  This file is part of the KDE project

    Copyright (c) 1999 Matthias Elter <elter@kde.org>
    Copyright (c) 2003 Patrick Julien <freak@codepimps.org>
    Copyright (c) 2005 Sven Langkamp <sven.langkamp@gmail.com>
    Copyright (C) 2011 Srikanth Tiyyagura <srikanth.tulasiram@gmail.com>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "KoResourceServerProvider.h"

#include <QApplication>
#include <QFileInfo>
#include <QStringList>
#include <QDir>
#include <QStandardPaths>
#include <QGlobalStatic>

#include <resources/KoSegmentGradient.h>
#include <resources/KoStopGradient.h>
#include "KoColorSpaceRegistry.h"
#include "KoResourcePaths.h"
#include "klocalizedstring.h"
#include <iostream>

using namespace std;

class GradientResourceServer : public KoResourceServer<KoAbstractGradient> {

public:

    GradientResourceServer(const QString& type)
        : KoResourceServer<KoAbstractGradient>(type)
    {
        insertSpecialGradients();
    }

    void insertSpecialGradients()
    {
        qDebug() << "insertSpecialGradients broken because we don't have a list we can insert in front of anymore";


        const KoColorSpace* cs = KoColorSpaceRegistry::instance()->rgb8();
        QList<KoGradientStop> stops;

        KoStopGradientSP gradient(new KoStopGradient());
        gradient->setType(QGradient::LinearGradient);
        gradient->setName(i18n("Foreground to Transparent"));
        stops << KoGradientStop(0.0, KoColor(Qt::black, cs)) << KoGradientStop(1.0, KoColor(QColor(0, 0, 0, 0), cs));

        gradient->setStops(stops);
        gradient->setValid(true);
        gradient->setPermanent(true);
        addResource(gradient, false);
        m_foregroundToTransparent = gradient;

        gradient.reset(new KoStopGradient());
        gradient->setType(QGradient::LinearGradient);
        gradient->setName(i18n("Foreground to Background"));

        stops.clear();
        stops << KoGradientStop(0.0, KoColor(Qt::black, cs)) << KoGradientStop(1.0, KoColor(Qt::white, cs));

        gradient->setStops(stops);
        gradient->setValid(true);
        gradient->setPermanent(true);
        addResource(gradient, false);
        m_foregroundToBackground = gradient;
    }

private:

    friend class KoResourceBundle;

    KoAbstractGradientSP createResource( const QString & filename ) {

        QString fileExtension;
        int index = filename.lastIndexOf('.');

        if (index != -1)
            fileExtension = filename.mid(index).toLower();

        KoAbstractGradientSP grad;

        if(fileExtension == ".svg" || fileExtension == ".kgr") {
            grad.reset(new KoStopGradient(filename));
        }
        else if(fileExtension == ".ggr" ) {
            grad.reset(new KoSegmentGradient(filename));
        }

        return grad;
    }

    KoAbstractGradientSP m_foregroundToTransparent;
    KoAbstractGradientSP m_foregroundToBackground;
};

struct Q_DECL_HIDDEN KoResourceServerProvider::Private
{
    KoResourceServer<KoPattern> *patternServer;
    KoResourceServer<KoAbstractGradient> *gradientServer;
    KoResourceServer<KoColorSet> *paletteServer;
    KoResourceServer<KoSvgSymbolCollectionResource> *svgSymbolCollectionServer;
    KoResourceServer<KoGamutMask> *gamutMaskServer;
};

KoResourceServerProvider::KoResourceServerProvider() : d(new Private)
{
    d->patternServer = new KoResourceServer<KoPattern>(ResourceType::Patterns);
    d->gradientServer = new GradientResourceServer(ResourceType::Gradients);
    d->paletteServer = new KoResourceServer<KoColorSet>(ResourceType::Palettes);
    d->svgSymbolCollectionServer = new KoResourceServer<KoSvgSymbolCollectionResource>(ResourceType::Symbols);
    d->gamutMaskServer = new KoResourceServer<KoGamutMask>(ResourceType::GamutMasks);
}

KoResourceServerProvider::~KoResourceServerProvider()
{
    delete d->patternServer;
    delete d->gradientServer;
    delete d->paletteServer;
    delete d->svgSymbolCollectionServer;
    delete d->gamutMaskServer;

    delete d;
}

Q_GLOBAL_STATIC(KoResourceServerProvider, s_instance)

KoResourceServerProvider *KoResourceServerProvider::instance()
{
    return s_instance;
}

QStringList KoResourceServerProvider::blacklistFileNames(QStringList fileNames, const QStringList &blacklistedFileNames)
{
    if (!blacklistedFileNames.isEmpty()) {
        foreach (const QString &s, blacklistedFileNames) {
            fileNames.removeAll(s);
        }
    }
    return fileNames;
}

KoResourceServer<KoPattern> *KoResourceServerProvider::patternServer()
{
    return d->patternServer;
}

KoResourceServer<KoAbstractGradient> *KoResourceServerProvider::gradientServer()
{
    return d->gradientServer;
}

KoResourceServer<KoColorSet> *KoResourceServerProvider::paletteServer()
{
    return d->paletteServer;
}

KoResourceServer<KoSvgSymbolCollectionResource> *KoResourceServerProvider::svgSymbolCollectionServer()
{
    return d->svgSymbolCollectionServer;
}

KoResourceServer<KoGamutMask> *KoResourceServerProvider::gamutMaskServer()
{
    return d->gamutMaskServer;
}


