/*
 * RelativePathsTest.cpp
 *
 * Copyright (c) 2017 Tres Finocchiaro <tres/dot/finocchiaro/at/gmail.com>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include <cstddef>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include "ConfigManager.h"
#include "Engine.h"
#include "PathUtil.h"
#include "Song.h"
#include "Track.h"

namespace
{
QString writeProjectFile(const QString& dirPath, const QString& tracksXml = QString())
{
	QFile project{QDir{dirPath}.filePath(QStringLiteral("project.mmp"))};
	if (!project.open(QIODevice::WriteOnly | QIODevice::Truncate)) { return {}; }

	QTextStream out{&project};
	out << "<?xml version=\"1.0\"?>\n"
		<< "<lmms-project version=\"9999\" type=\"song\" creator=\"LMMS test\" creatorversion=\"1.3.0\">\n"
		<< "<head timesig_numerator=\"4\" timesig_denominator=\"4\" mastervol=\"100\" bpm=\"140\" masterpitch=\"0\"/>\n"
		<< "<song>\n"
		<< "<trackcontainer width=\"600\" x=\"5\" y=\"5\" maximized=\"0\" height=\"300\" visible=\"1\" type=\"song\" minimized=\"0\">\n"
		<< tracksXml
		<< "</trackcontainer>\n"
		<< "</song>\n"
		<< "</lmms-project>\n";
	project.close();
	return project.fileName();
}
} // namespace


class RelativePathsTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		lmms::Engine::init(true);
	}

	void cleanupTestCase()
	{
		lmms::Engine::destroy();
	}

	void PathUtilComparisonTests()
	{
		using namespace lmms;

		QFileInfo fi(ConfigManager::inst()->factorySamplesDir() + "/drums/kick01.ogg");
		QVERIFY(fi.exists());

		QString absPath = fi.absoluteFilePath();
		QString oldRelPath = "drums/kick01.ogg";
		QString relPath = PathUtil::basePrefix(PathUtil::Base::FactorySample) + "drums/kick01.ogg";
		QString fuzPath = absPath;
		fuzPath.replace(relPath, "drums/.///kick01.ogg");

		//Test nicely formatted paths
		QCOMPARE(PathUtil::toShortestRelative(absPath), relPath);
		QCOMPARE(PathUtil::toAbsolute(relPath), absPath);

		//Test upgrading old paths. Known prefixes must keep their old behaviour.
		QCOMPARE(PathUtil::toShortestRelative(oldRelPath), relPath);
		QCOMPARE(PathUtil::toAbsolute(oldRelPath), absPath);

		//Test weird but valid paths
		QCOMPARE(PathUtil::toShortestRelative(fuzPath), relPath);
		QCOMPARE(PathUtil::toAbsolute(fuzPath), absPath);

		//Empty paths should stay empty
		QString empty = QString("");
		QCOMPARE(PathUtil::stripPrefix(""), empty);
		QCOMPARE(PathUtil::cleanName(""), empty);
		QCOMPARE(PathUtil::toAbsolute(""), empty);
		QCOMPARE(PathUtil::toShortestRelative(""), empty);
	}

	// R01: prefix-less relative paths resolve against the project directory.
	void ProjectLocalSamplePaths()
	{
		using namespace lmms;

		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		QVERIFY(QDir{dir.path()}.mkpath(QStringLiteral("samples")));

		QFile sample{dir.filePath(QStringLiteral("samples/tick1.wav"))};
		QVERIFY(sample.open(QIODevice::WriteOnly));
		sample.close();

		const QString projectFile = writeProjectFile(dir.path());
		QVERIFY(!projectFile.isEmpty());
		Engine::getSong()->loadProject(projectFile);

		const QString expected = QFileInfo{dir.filePath(QStringLiteral("samples/tick1.wav"))}.absoluteFilePath();
		QCOMPARE(PathUtil::toAbsolute(QStringLiteral("samples/tick1.wav")), expected);

		// An explicit "local:" prefix keeps working as before.
		QCOMPARE(PathUtil::toAbsolute(QStringLiteral("local:samples/tick1.wav")), expected);
	}

	// R01: a project-local file that cannot be found must warn but not crash.
	void MissingProjectLocalSampleDoesNotCrash()
	{
		using namespace lmms;

		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		const QString projectFile = writeProjectFile(dir.path());
		QVERIFY(!projectFile.isEmpty());
		Engine::getSong()->loadProject(projectFile);

		const QString missing = QStringLiteral("samples/this-file-does-not-exist.wav");
		const QString resolved = PathUtil::toAbsolute(missing);

		// Not resolved as an existing project-local file and no crash.
		const QString projectCandidate =
			QFileInfo{QDir{dir.path()}.filePath(missing)}.absoluteFilePath();
		QVERIFY(resolved != projectCandidate);
	}

	// R11: type 6 (HiddenAutomation) tracks in a track container are rendered
	// like type 5 (Automation) tracks.
	void HiddenAutomationTracksAreLoadedAsAutomation()
	{
		using namespace lmms;

		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		const QString hiddenTrack = QStringLiteral(
			"<track muted=\"0\" type=\"6\" name=\"Hidden automation\">\n"
			"<automationtrack/>\n"
			"</track>\n");
		const QString projectFile = writeProjectFile(dir.path(), hiddenTrack);
		QVERIFY(!projectFile.isEmpty());

		Engine::getSong()->loadProject(projectFile);

		const auto& tracks = Engine::getSong()->tracks();
		QCOMPARE(tracks.size(), std::size_t{1});
		QVERIFY(tracks.front()->type() == Track::Type::Automation);

		// Loading must not rewrite the user's project file.
		QFile original{projectFile};
		QVERIFY(original.open(QIODevice::ReadOnly));
		const QByteArray contents = original.readAll();
		original.close();
		QVERIFY(contents.contains("type=\"6\""));
	}

	// R07: the tick based export range API.
	void ExportRangeApi()
	{
		using namespace lmms;

		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString projectFile = writeProjectFile(dir.path());
		QVERIFY(!projectFile.isEmpty());

		auto* song = Engine::getSong();
		song->loadProject(projectFile);

		QVERIFY(!song->hasExportRange());

		const int startTicks = 50000;
		const int endTicks = 100000;
		song->setExportRange(startTicks, endTicks);
		QVERIFY(song->hasExportRange());

		song->startExport();
		const auto playPos = song->getPlayPos(Song::PlayMode::Song).getTicks();
		QVERIFY(playPos >= startTicks);
		QVERIFY(playPos < endTicks);
		QVERIFY(!song->isExportDone());

		song->setPlayPos(endTicks, Song::PlayMode::Song);
		QVERIFY(song->isExportDone());
		QCOMPARE(song->getExportProgress(), 100);
		song->stopExport();

		song->clearExportRange();
		QVERIFY(!song->hasExportRange());
	}

	// R14: saving a project twice yields a stable mixer/master section.
	void MixerSerializationIsStable()
	{
		using namespace lmms;

		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString projectFile = writeProjectFile(dir.path());
		QVERIFY(!projectFile.isEmpty());

		auto* song = Engine::getSong();
		song->loadProject(projectFile);

		const QString firstSave = dir.filePath(QStringLiteral("first.mmp"));
		const QString secondSave = dir.filePath(QStringLiteral("second.mmp"));
		QVERIFY(song->saveProjectFile(firstSave));
		song->loadProject(firstSave);
		QVERIFY(song->saveProjectFile(secondSave));

		const auto mixerFrom = [](const QString& fileName) {
			QDomDocument doc;
			QFile file{fileName};
			if (!file.open(QIODevice::ReadOnly)) { return QDomElement{}; }
			if (!doc.setContent(&file)) { return QDomElement{}; }
			return doc.documentElement()
				.firstChildElement(QStringLiteral("song"))
				.firstChildElement(QStringLiteral("mixer"));
		};

		const auto firstMixer = mixerFrom(firstSave);
		const auto secondMixer = mixerFrom(secondSave);
		QVERIFY(!firstMixer.isNull());
		QVERIFY(!secondMixer.isNull());
		const auto elementToString = [](const QDomElement& element) {
			QDomDocument doc;
			doc.appendChild(doc.importNode(element, true));
			return QString::fromUtf8(doc.toByteArray());
		};
		QCOMPARE(elementToString(firstMixer), elementToString(secondMixer));

		// The implicit master channel is always written explicitly, exactly once.
		const auto channels = firstMixer.elementsByTagName(QStringLiteral("mixerchannel"));
		int masterCount = 0;
		for (int i = 0; i < channels.count(); ++i)
		{
			const auto channel = channels.at(i).toElement();
			if (channel.attribute(QStringLiteral("num")).toInt() == 0)
			{
				++masterCount;
				QCOMPARE(channel.attribute(QStringLiteral("name")), QStringLiteral("Master"));
			}
		}
		QCOMPARE(masterCount, 1);
	}
};

QTEST_GUILESS_MAIN(RelativePathsTest)
#include "RelativePathsTest.moc"
