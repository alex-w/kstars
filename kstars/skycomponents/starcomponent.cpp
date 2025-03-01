/*
    SPDX-FileCopyrightText: 2005 Thomas Kabelmann <thomas.kabelmann@gmx.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "starcomponent.h"

#include "binfilehelper.h"
#include "deepstarcomponent.h"
#include "highpmstarlist.h"
#ifndef KSTARS_LITE
#include "kstars.h"
#endif
#include "kstarsdata.h"
#include "kstarssplash.h"
#include "Options.h"
#include "skylabeler.h"
#include "skymap.h"
#include "skymesh.h"
#ifndef KSTARS_LITE
#include "skyqpainter.h"
#endif
#include "htmesh/MeshIterator.h"
#include "projections/projector.h"

#include "kstars_debug.h"

#include <qplatformdefs.h>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(Q_OS_FREEBSD) || defined(Q_OS_NETBSD)
#include <sys/endian.h>
#define bswap_16(x) bswap16(x)
#define bswap_32(x) bswap32(x)
#else
#include "byteorder.h"
#endif

// Qt version calming
#include <qtskipemptyparts.h>

StarComponent *StarComponent::pinstance = nullptr;

StarComponent::StarComponent(SkyComposite *parent)
    : ListComponent(parent), m_reindexNum(J2000)
{
    m_skyMesh          = SkyMesh::Instance();
    m_StarBlockFactory = StarBlockFactory::Instance();

    m_starIndex.reset(new StarIndex());
    for (int i = 0; i < m_skyMesh->size(); i++)
        m_starIndex->append(new StarList());
    m_highPMStars.append(new HighPMStarList(840.0));
    m_highPMStars.append(new HighPMStarList(304.0));
    m_reindexInterval = StarObject::reindexInterval(304.0);

    for (int i = 0; i <= MAX_LINENUMBER_MAG; i++)
        m_labelList[i] = new LabelList;

    // Actually load data
    emitProgressText(i18n("Loading stars"));

    loadStaticData();
    // Load any deep star catalogs that are available
    loadDeepStarCatalogs();

    // The following works but can cause crashes sometimes
    //QtConcurrent::run(this, &StarComponent::loadDeepStarCatalogs);

    //In KStars Lite star images are initialized in SkyMapLite
#ifndef KSTARS_LITE
    SkyQPainter::initStarImages();
#endif
}

StarComponent::~StarComponent()
{
    qDeleteAll(*m_starIndex);
    m_starIndex->clear();
    qDeleteAll(m_DeepStarComponents);
    m_DeepStarComponents.clear();
    qDeleteAll(m_highPMStars);
    m_highPMStars.clear();
    for (int i = 0; i <= MAX_LINENUMBER_MAG; i++)
        delete m_labelList[i];
}

StarComponent *StarComponent::Create(SkyComposite *parent)
{
    delete pinstance;
    pinstance = new StarComponent(parent);
    return pinstance;
}

bool StarComponent::selected()
{
    return Options::showStars();
}

bool StarComponent::addDeepStarCatalogIfExists(const QString &fileName, float trigMag, bool staticstars)
{
    if (BinFileHelper::testFileExists(fileName))
    {
        m_DeepStarComponents.append(new DeepStarComponent(parent(), fileName, trigMag, staticstars));
        return true;
    }
    return false;
}

int StarComponent::loadDeepStarCatalogs()
{
    // Look for the basic unnamed star catalog to mag 8.0
    if (!addDeepStarCatalogIfExists("unnamedstars.dat", -5.0, true))
        return 0;

    // Look for the Tycho-2 add-on with 2.5 million stars to mag 12.5
    if (!addDeepStarCatalogIfExists("tycho2.dat", 8.0) && !addDeepStarCatalogIfExists("deepstars.dat", 8.0))
        return 1;

    // Look for the USNO NOMAD 1e8 star catalog add-on with stars to mag 16
    if (!addDeepStarCatalogIfExists("USNO-NOMAD-1e8.dat", 11.0))
        return 2;

    return 3;
}

//This function is empty for a reason; we override the normal
//update function in favor of JiT updates for stars.
void StarComponent::update(KSNumbers *)
{
}

// We use the update hook to re-index all the stars when the date has changed by
// more than 150 years.

bool StarComponent::reindex(KSNumbers *num)
{
    if (!num)
        return false;

    // for large time steps we re-index all points
    if (fabs(num->julianCenturies() - m_reindexNum.julianCenturies()) > m_reindexInterval)
    {
        reindexAll(num);
        return true;
    }

    bool highPM = true;

    // otherwise we just re-index fast movers as needed
    for (auto &star : m_highPMStars)
        highPM &= !(star->reindex(num, m_starIndex.get()));

    return !(highPM);
}

void StarComponent::reindexAll(KSNumbers *num)
{
#if 0
    if (0 && !m_reindexSplash)
    {
        m_reindexSplash = new KStarsSplash(i18n("Please wait while re-indexing stars..."));
        QObject::connect(KStarsData::Instance(), SIGNAL(progressText(QString)), m_reindexSplash,
                         SLOT(setMessage(QString)));

        m_reindexSplash->show();
        m_reindexSplash->raise();
        return;
    }
#endif

    qCInfo(KSTARS) << "Re-indexing Stars to year" << 2000.0 + num->julianCenturies() * 100.0;

    m_reindexNum = KSNumbers(*num);
    m_skyMesh->setKSNumbers(num);

    // clear out the old index
    for (auto &item : *m_starIndex)
    {
        item->clear();
    }

    // re-populate it from the objectList
    for (auto &object : m_ObjectList)
    {
        StarObject *star = dynamic_cast<StarObject *>(object);
        Trixel trixel    = m_skyMesh->indexStar(star);

        m_starIndex->at(trixel)->append(star);
    }

    // Let everyone else know we have re-indexed to num
    for (auto &star : m_highPMStars)
    {
        star->setIndexTime(num);
    }

    //delete m_reindexSplash;
    //m_reindexSplash = 0;
}

float StarComponent::faintMagnitude() const
{
    float faintmag = m_FaintMagnitude;

    for (auto &component : m_DeepStarComponents)
    {
        if (faintmag < component->faintMagnitude())
            faintmag = component->faintMagnitude();
    }
    return faintmag;
}

float StarComponent::zoomMagnitudeLimit()
{
    //adjust maglimit for ZoomLevel
    double lgmin = log10(MINZOOM);
    double lgz   = log10(Options::zoomFactor());

    // Old formula:
    //    float maglim = ( 2.000 + 2.444 * Options::memUsage() / 10.0 ) * ( lgz - lgmin ) + Options::magLimitDrawStarZoomOut();

    /*
     Explanation for the following formula:
     --------------------------------------
     Estimates from a sample of 125000 stars shows that, magnitude
     limit vs. number of stars follows the formula:
       nStars = 10^(.45 * maglim + .95)
     (A better formula is available here: https://www.aa.quae.nl/en/antwoorden/magnituden.html
      which we do not implement for simplicity)
     We want to keep the star density on screen a constant. This is directly proportional to the number of stars
     and directly proportional to the area on screen. The area is in turn inversely proportional to the square
     of the zoom factor ( zoomFactor / MINZOOM ). This means that (taking logarithms):
       0.45 * maglim + 0.95 - 2 * log( ZoomFactor ) - log( Star Density ) - log( Some proportionality constant )
     hence the formula. We've gathered together all the constants and set it to 3.5, so as to set the minimum
     possible value of maglim to 3.5
    */

    //    float maglim = 4.444 * ( lgz - lgmin ) + 2.222 * log10( Options::starDensity() ) + 3.5;

    // Reducing the slope w.r.t zoom factor to avoid the extremely fast increase in star density with zoom
    // that 4.444 gives us (although that is what the derivation gives us)

    return 3.5 + 3.7 * (lgz - lgmin) + 2.222 * log10(static_cast<float>(Options::starDensity()));
}

void StarComponent::draw(SkyPainter *skyp)
{
#ifndef KSTARS_LITE
    if (!selected())
        return;

    // If we are displaying HIPS, the it may not make sense to also render stars.
    if (Options::showHIPS() && !Options::showStarsOverHIPS())
        return;

    SkyMap *map           = SkyMap::Instance();
    const Projector *proj = map->projector();
    KStarsData *data      = KStarsData::Instance();
    UpdateID updateID     = data->updateID();

    bool checkSlewing = (map->isSlewing() && Options::hideOnSlew());
    m_hideLabels      = checkSlewing || !(Options::showStarMagnitudes() || Options::showStarNames());

    //shortcuts to inform whether to draw different objects
    bool hideFaintStars = checkSlewing && Options::hideStars();
    double hideStarsMag = Options::magLimitHideStar();
    reindex(data->updateNum());

    double lgmin = log10(MINZOOM);
    double lgmax = log10(MAXZOOM);
    double lgz   = log10(Options::zoomFactor());

    double maglim;
    m_zoomMagLimit = maglim = zoomMagnitudeLimit();

    double labelMagLim = Options::starLabelDensity() / 5.0;
    labelMagLim += (12.0 - labelMagLim) * (lgz - lgmin) / (lgmax - lgmin);
    if (labelMagLim > 8.0)
        labelMagLim = 8.0;

    //Calculate sizeMagLim
    // Old formula:
    //    float sizeMagLim = ( 2.000 + 2.444 * Options::memUsage() / 10.0 ) * ( lgz - lgmin ) + 5.8;

    // Using the maglim to compute the sizes of stars reduces
    // discernability between brighter and fainter stars at high zoom
    // levels. To fix that, we use an "arbitrary" constant in place of
    // the variable star density.
    // Not using this formula now.
    //    float sizeMagLim = 4.444 * ( lgz - lgmin ) + 5.0;

    float sizeMagLim = zoomMagnitudeLimit();
    if (sizeMagLim > faintMagnitude() * (1 - 1.5 / 16))
        sizeMagLim = faintMagnitude() * (1 - 1.5 / 16);
    skyp->setSizeMagLimit(sizeMagLim);

    //Loop for drawing star images

    MeshIterator region(m_skyMesh, DRAW_BUF);
    magLim = maglim;

    // If we are hiding faint stars, then maglim is really the brighter of hideStarsMag and maglim
    if (hideFaintStars && maglim > hideStarsMag)
        maglim = hideStarsMag;

    m_StarBlockFactory->drawID = m_skyMesh->drawID();

    int nTrixels = 0;

    while (region.hasNext())
    {
        ++nTrixels;
        Trixel currentRegion = region.next();
        StarList *starList   = m_starIndex->at(currentRegion);

        for (auto &star : *starList)
        {
            if (!star)
                continue;

            float mag = star->mag();

            // break loop if maglim is reached
            if (mag > maglim)
                break;

            if (star->updateID != updateID)
                star->JITupdate();

            bool drawn = skyp->drawPointSource(star, mag, star->spchar());

            //FIXME_SKYPAINTER: find a better way to do this.
            if (drawn && !(m_hideLabels || mag > labelMagLim))
                addLabel(proj->toScreen(star), star);
        }
    }

    // Draw focusStar if not null
    if (focusStar)
    {
        if (focusStar->updateID != updateID)
            focusStar->JITupdate();
        float mag = focusStar->mag();
        skyp->drawPointSource(focusStar, mag, focusStar->spchar());
    }

    // Now draw each of our DeepStarComponents
    for (auto &component : m_DeepStarComponents)
    {
        component->draw(skyp);
    }
#else
    Q_UNUSED(skyp)
#endif
}

void StarComponent::addLabel(const QPointF &p, StarObject *star)
{
    int idx = int(star->mag() * 10.0);
    if (idx < 0)
        idx = 0;
    if (idx > MAX_LINENUMBER_MAG)
        idx = MAX_LINENUMBER_MAG;
    m_labelList[idx]->append(SkyLabel(p, star));
}

void StarComponent::drawLabels()
{
    if (m_hideLabels)
        return;

    SkyLabeler *labeler = SkyLabeler::Instance();
    labeler->setPen(QColor(KStarsData::Instance()->colorScheme()->colorNamed("SNameColor")));

    int max = int(m_zoomMagLimit * 10.0);
    if (max < 0)
        max = 0;
    if (max > MAX_LINENUMBER_MAG)
        max = MAX_LINENUMBER_MAG;

    for (int i = 0; i <= max; i++)
    {
        LabelList *list = m_labelList[i];

        for (const auto &item : *list)
        {
            labeler->drawNameLabel(item.obj, item.o);
        }
        list->clear();
    }
}

bool StarComponent::loadStaticData()
{
    // We break from Qt / KDE API and use traditional file handling here, to obtain speed.
    // We also avoid C++ constructors for the same reason.
    FILE *dataFile, *nameFile;
    bool swapBytes = false, named = false, gnamed = false;
    BinFileHelper dataReader, nameReader;
    QString name, gname, visibleName;
    StarObject *star;

    if (starsLoaded)
        return true;

    // prepare to index stars to this date
    m_skyMesh->setKSNumbers(&m_reindexNum);

    /* Open the data files */
    // TODO: Maybe we don't want to hardcode the filename?
    if ((dataFile = dataReader.openFile("namedstars.dat")) == nullptr)
    {
        qCWarning(KSTARS) << "Could not open data file namedstars.dat";
        return false;
    }

    if (!(nameFile = nameReader.openFile("starnames.dat")))
    {
        qCWarning(KSTARS) << "Could not open data file starnames.dat";
        return false;
    }

    if (!dataReader.readHeader())
    {
        qCWarning(KSTARS) << "Error reading namedstars.dat header : " << dataReader.getErrorNumber() << " : "
                          << dataReader.getError();
        return false;
    }

    if (!nameReader.readHeader())
    {
        qCWarning(KSTARS) << "Error reading starnames.dat header : " << nameReader.getErrorNumber() << " : "
                          << nameReader.getError();
        return false;
    }
    //KDE_fseek(nameFile, nameReader.getDataOffset(), SEEK_SET);
    QT_FSEEK(nameFile, nameReader.getDataOffset(), SEEK_SET);
    swapBytes = dataReader.getByteSwap();

    long int nstars = 0;

    //KDE_fseek(dataFile, dataReader.getDataOffset(), SEEK_SET);
    QT_FSEEK(dataFile, dataReader.getDataOffset(), SEEK_SET);

    qint16 faintmag;
    quint8 htm_level;
    quint16 t_MSpT;
    int ret = 0;

    // Faint Magnitude
    ret = fread(&faintmag, 2, 1, dataFile);
    if (swapBytes)
        faintmag = bswap_16(faintmag);

    // HTM Level
    ret = fread(&htm_level, 1, 1, dataFile);

    // Unused
    {
        int rc = fread(&t_MSpT, 2, 1, dataFile);
        Q_UNUSED(rc)
    }

    if (faintmag / 100.0 > m_FaintMagnitude)
        m_FaintMagnitude = faintmag / 100.0;

    if (htm_level != m_skyMesh->level())
        qCWarning(KSTARS)
                << "HTM Level in shallow star data file and HTM Level in m_skyMesh do not match. EXPECT TROUBLE"
                ;
    for (int i = 0; i < m_skyMesh->size(); ++i)
    {
        Trixel trixel = i; // = ( ( i >= 256 ) ? ( i - 256 ) : ( i + 256 ) );
        for (unsigned long j = 0; j < static_cast<unsigned long>(dataReader.getRecordCount(i)); ++j)
        {
            if (1 != fread(&stardata, sizeof(StarData), 1, dataFile))
            {
                qCCritical(KSTARS) << "FILE FORMAT ERROR: Could not read StarData structure for star #" << j << " under trixel #"
                                   << trixel;
                continue;
            }

            /* Swap Bytes when required */
            if (swapBytes)
                byteSwap(&stardata);

            named  = false;
            gnamed = false;

            /* Named Star - Read the nameFile */
            if (stardata.flags & 0x01)
            {
                visibleName = "";
                if (1 != fread(&starname, sizeof(starName), 1, nameFile))
                    qCCritical(KSTARS) << "ERROR: fread() call on nameFile failed in trixel " << trixel << " star " << j;

                name  = QString::fromLatin1(starname.longName);
                named = !name.isEmpty();

                gname  = QString::fromLatin1(starname.bayerName);
                gnamed = !gname.isEmpty();

                if (gnamed && starname.bayerName[0] != '.')
                    visibleName = gname;

                if (named)
                {
                    // HEV: look up star name in internationalization filesource
                    name = i18nc("star name", name.toLocal8Bit().data());
                }
                else
                {
                    name = i18n("star");
                }
            }
            else
                qCCritical(KSTARS) << "ERROR: Named star file contains unnamed stars! Expect trouble.";

            /* Create the new StarObject */
            star = new StarObject;
            star->init(&stardata);
            //if( star->getHDIndex() != 0 && name == i18n("star"))
            if (stardata.HD)
            {
                m_HDHash.insert(stardata.HD, star);
                if (named == false)
                {
                    name  = QString("HD %1").arg(stardata.HD);
                    named = true;
                }
            }

            star->setNames(name, visibleName);
            //star->EquatorialToHorizontal( data->lst(), data->geo()->lat() );
            ++nstars;

            if (gnamed)
                m_genName.insert(gname, star);

            //if ( ! name.isEmpty() && name != i18n("star"))
            if (named)
            {
                objectNames(SkyObject::STAR).append(name);
                objectLists(SkyObject::STAR).append(QPair<QString, const SkyObject *>(name, star));
            }

            if (!visibleName.isEmpty() && gname != name)
            {
                QString gName = star->gname(false);
                objectNames(SkyObject::STAR).append(gName);
                objectLists(SkyObject::STAR).append(QPair<QString, const SkyObject *>(gName, star));
            }

            appendListObject(star);

            m_starIndex->at(trixel)->append(star);
            double pm = star->pmMagnitude();

            for (auto &list : m_highPMStars)
            {
                if (list->append(trixel, star, pm))
                    break;
            }
        }
    }

    dataReader.closeFile();
    nameReader.closeFile();

    starsLoaded = true;
    return true;
}

void StarComponent::appendListObject(SkyObject *object)
{
    m_ObjectList.append(object);
    m_ObjectHash.insert(object->name().toLower(), object);
    m_ObjectHash.insert(object->longname().toLower(), object);
    m_ObjectHash.insert(object->name2().toLower(), object);
    m_ObjectHash.insert(object->name2().toLower(), object);
    m_ObjectHash.insert((dynamic_cast<StarObject *>(object))->gname(false).toLower(), object);
}

SkyObject *StarComponent::findStarByGenetiveName(const QString name)
{
    if (name.startsWith(QLatin1String("HD")))
    {
        QStringList fields = name.split(' ', Qt::SkipEmptyParts);
        bool Ok            = false;
        unsigned int HDNum = fields[1].toInt(&Ok);
        if (Ok)
            return findByHDIndex(HDNum);
    }
    return m_genName.value(name);
}

void StarComponent::objectsInArea(QList<SkyObject *> &list, const SkyRegion &region)
{
    for (SkyRegion::const_iterator it = region.constBegin(); it != region.constEnd(); ++it)
    {
        Trixel trixel      = it.key();
        StarList *starlist = m_starIndex->at(trixel);
        for (int i = 0; starlist && i < starlist->size(); i++)
            if (starlist->at(i) && starlist->at(i)->name() != QString("star"))
                list.push_back(starlist->at(i));
    }
}

StarObject *StarComponent::findByHDIndex(int HDnum)
{
    KStarsData *data = KStarsData::Instance();
    StarObject *o = nullptr;
    BinFileHelper hdidxReader;

    // First check the hash to see if we have a corresponding StarObject already
    if ((o = m_HDHash.value(HDnum, nullptr)))
        return o;
    // If we don't have the StarObject here, try it in the DeepStarComponents' hashes
    if (m_DeepStarComponents.size() >= 1)
        if ((o = m_DeepStarComponents.at(0)->findByHDIndex(HDnum)))
            return o;
    if (m_DeepStarComponents.size() >= 2)
    {
        qint32 offset = 0;
        int ret = 0;
        FILE *hdidxFile = hdidxReader.openFile("Henry-Draper.idx");
        FILE *dataFile = nullptr;

        if (!hdidxFile)
            return nullptr;
        //KDE_fseek( hdidxFile, (HDnum - 1) * 4, SEEK_SET );
        QT_FSEEK(hdidxFile, (HDnum - 1) * 4, SEEK_SET);
        // TODO: Offsets need to be byteswapped if this is a big endian machine.
        // This means that the Henry Draper Index needs a endianness indicator.
        ret = fread(&offset, 4, 1, hdidxFile);
        if (offset <= 0)
            return nullptr;
        dataFile = m_DeepStarComponents.at(1)->getStarReader()->getFileHandle();
        //KDE_fseek( dataFile, offset, SEEK_SET );
        QT_FSEEK(dataFile, offset, SEEK_SET);
        {
            int rc = fread(&stardata, sizeof(StarData), 1, dataFile);
            Q_UNUSED(rc)
        }
        if (m_DeepStarComponents.at(1)->getStarReader()->getByteSwap())
        {
            byteSwap(&stardata);
        }
        m_starObject.init(&stardata);
        m_starObject.EquatorialToHorizontal(data->lst(), data->geo()->lat());
        m_starObject.JITupdate();
        focusStar = m_starObject.clone();
        m_HDHash.insert(HDnum, focusStar);
        hdidxReader.closeFile();
        return focusStar;
    }

    return nullptr;
}

// This uses the main star index for looking up nearby stars but then
// filters out objects with the generic name "star".  We could easily
// build an index for just the named stars which would make this go
// much faster still.  -jbb
//
SkyObject *StarComponent::objectNearest(SkyPoint *p, double &maxrad)
{
    m_zoomMagLimit = zoomMagnitudeLimit();

    SkyObject *oBest = nullptr;

    MeshIterator region(m_skyMesh, OBJ_NEAREST_BUF);

    while (region.hasNext())
    {
        Trixel currentRegion = region.next();
        StarList *starList   = m_starIndex->at(currentRegion);

        for (auto &star : *starList)
        {
            if (!star)
                continue;
            if (star->mag() > m_zoomMagLimit)
                continue;

            double r = star->angularDistanceTo(p).Degrees();

            if (r < maxrad)
            {
                oBest  = star;
                maxrad = r;
            }
        }
    }

    // Check up with our Deep Star Components too!
    double rTry, rBest;
    SkyObject *oTry;
    // JM 2016-03-30: Multiply rBest by a factor of 0.5 so that named stars are preferred to unnamed stars searched below
    rBest = maxrad * 0.5;
    rTry  = maxrad;
    for (auto &component : m_DeepStarComponents)
    {
        oTry = component->objectNearest(p, rTry);
        if (rTry < rBest)
        {
            rBest = rTry;
            oBest = oTry;
        }
    }
    maxrad = rBest;

    return oBest;
}

void StarComponent::starsInAperture(QList<StarObject *> &list, const SkyPoint &center, float radius, float maglim)
{
    // Ensure that we have deprecessed the (RA, Dec) to (RA0, Dec0)
    Q_ASSERT(center.ra0().Degrees() >= 0.0);
    Q_ASSERT(center.dec0().Degrees() <= 90.0);

    m_skyMesh->intersect(center.ra0().Degrees(), center.dec0().Degrees(), radius, static_cast<BufNum>(OBJ_NEAREST_BUF));

    MeshIterator region(m_skyMesh, OBJ_NEAREST_BUF);

    if (maglim < -28)
        maglim = m_FaintMagnitude;

    while (region.hasNext())
    {
        Trixel currentRegion = region.next();
        StarList *starList   = m_starIndex->at(currentRegion);

        for (auto &star : *starList)
        {
            if (!star)
                continue;
            if (star->mag() > m_FaintMagnitude)
                continue;
            if (star->angularDistanceTo(&center).Degrees() <= radius)
                list.append(star);
        }
    }

    // Add stars from the DeepStarComponents as well
    for (auto &component : m_DeepStarComponents)
    {
        component->starsInAperture(list, center, radius, maglim);
    }
}

void StarComponent::byteSwap(StarData *stardata)
{
    stardata->RA       = bswap_32(stardata->RA);
    stardata->Dec      = bswap_32(stardata->Dec);
    stardata->dRA      = bswap_32(stardata->dRA);
    stardata->dDec     = bswap_32(stardata->dDec);
    stardata->parallax = bswap_32(stardata->parallax);
    stardata->HD       = bswap_32(stardata->HD);
    stardata->mag      = bswap_16(stardata->mag);
    stardata->bv_index = bswap_16(stardata->bv_index);
}
/*
void StarComponent::printDebugInfo() {

    int nTrixels = 0;
    int nBlocks = 0;
    long int nStars = 0;
    float faintMag = -5.0;

    MeshIterator trixels( m_skyMesh, DRAW_BUF );
    Trixel trixel;

    while( trixels.hasNext() ) {
        trixel = trixels.next();
        nTrixels++;
        for(int i = 0; i < m_starBlockList[ trixel ]->getBlockCount(); ++i) {
            nBlocks++;
            StarBlock *block = m_starBlockList[ trixel ]->block( i );
            for(int j = 0; j < block->getStarCount(); ++j) {
                nStars++;
            }
            if( block->getFaintMag() > faintMag ) {
                faintMag = block->getFaintMag();
            }
        }
    }

    printf( "========== UNNAMED STAR MEMORY ALLOCATION INFORMATION ==========\n" );
    printf( "Number of visible trixels                    = %8d\n", nTrixels );
    printf( "Number of visible StarBlocks                 = %8d\n", nBlocks );
    printf( "Number of StarBlocks allocated via SBF       = %8d\n", m_StarBlockFactory.getBlockCount() );
    printf( "Number of unnamed stars in memory            = %8ld\n", nStars );
    printf( "Magnitude of the faintest star in memory     = %8.2f\n", faintMag );
    printf( "Target magnitude limit                       = %8.2f\n", magLim );
    printf( "Size of each StarBlock                       = %8d bytes\n", sizeof( StarBlock ) );
    printf( "Size of each StarObject                      = %8d bytes\n", sizeof( StarObject ) );
    printf( "Memory use due to visible unnamed stars      = %8.2f MB\n", ( sizeof( StarObject ) * nStars / 1048576.0 ) );
    printf( "Memory use due to visible StarBlocks         = %8d bytes\n", sizeof( StarBlock ) * nBlocks );
    printf( "Memory use due to StarBlocks in SBF          = %8d bytes\n", sizeof( StarBlock ) * m_StarBlockFactory.getBlockCount() );
    printf( "=============== STAR DRAW LOOP TIMING INFORMATION ==============\n" );
    printf( "Time taken for drawing named stars           = %8ld ms\n", t_drawNamed );
    printf( "Time taken for dynamic load of data          = %8ld ms\n", t_dynamicLoad );
    printf( "Time taken for updating LRU cache            = %8ld ms\n", t_updateCache );
    printf( "Time taken for drawing unnamed stars         = %8ld ms\n", t_drawUnnamed );
    printf( "================================================================\n" );
}

bool StarComponent::verifySBLIntegrity() {

    float faintMag = -5.0;
    bool integrity = true;
    for(Trixel trixel = 0; trixel < m_skyMesh->size(); ++trixel) {
        for(int i = 0; i < m_starBlockList[ trixel ]->getBlockCount(); ++i) {
            StarBlock *block = m_starBlockList[ trixel ]->block( i );
            if( i == 0 )
                faintMag = block->getBrightMag();
            // NOTE: Assumes 2 decimal places in magnitude field. TODO: Change if it ever does change
            if( block->getBrightMag() != faintMag && ( block->getBrightMag() - faintMag ) > 0.016) {
                qDebug() << Q_FUNC_INFO << "Trixel " << trixel << ": ERROR: faintMag of prev block = " << faintMag
                         << ", brightMag of block #" << i << " = " << block->getBrightMag();
                integrity = false;
            }
            if( i > 1 && ( !block->prev ) )
                qDebug() << Q_FUNC_INFO << "Trixel " << trixel << ": ERROR: Block" << i << "is unlinked in LRU Cache";
            if( block->prev && block->prev->parent == m_starBlockList[ trixel ]
                && block->prev != m_starBlockList[ trixel ]->block( i - 1 ) ) {
                qDebug() << Q_FUNC_INFO << "Trixel " << trixel << ": ERROR: SBF LRU Cache linked list seems to be broken at before block " << i;
                integrity = false;
            }
            faintMag = block->getFaintMag();
        }
    }
    return integrity;
}
*/
