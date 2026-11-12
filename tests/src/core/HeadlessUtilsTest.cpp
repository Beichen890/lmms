/*
 * HeadlessUtilsTest.cpp - tests for the headless CLI helpers
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "../../../src/core/HeadlessUtils.h"

#include <QtTest>

class HeadlessUtilsTest : public QObject
{
	Q_OBJECT
private slots:
	void testTimeSpec()
	{
		using namespace lmms::headless;

		int ticks = -1;
		QString error;

		QVERIFY(parseTimeSpec("768", 192, 4, &ticks, &error));
		QCOMPARE(ticks, 768);

		QVERIFY(parseTimeSpec("4 bars", 192, 4, &ticks, &error));
		QCOMPARE(ticks, 768);

		QVERIFY(parseTimeSpec("4 beats", 192, 4, &ticks, &error));
		QCOMPARE(ticks, 192);

		QVERIFY(parseTimeSpec("1.5 bars", 192, 4, &ticks, &error));
		QCOMPARE(ticks, 288);

		QVERIFY(!parseTimeSpec("abc", 192, 4, &ticks, &error));
		QVERIFY(!parseTimeSpec("1.5", 192, 4, &ticks, &error));
	}

	void testCanonicalXml()
	{
		using namespace lmms::headless;

		const QByteArray input =
			"<?xml version=\"1.0\"?>"
			"<lmms-project version=\"1.0\">"
			"<song b=\"2\" a=\"1\">"
			"<track name=\"B\"/><track name=\"A\"/>"
			"</song>"
			"<mixer>"
			"<mixerchannel num=\"0\" name=\"Master\" volume=\"100\">"
			"<fxchain numofeffects=\"0\"/>"
			"</mixerchannel>"
			"</mixer>"
			"</lmms-project>";

		QString error;
		const QString first = canonicalizeXml(input, true, true, &error);
		QVERIFY2(!first.isEmpty(), qPrintable(error));
		const QString second = canonicalizeXml(input, true, true, &error);
		QCOMPARE(first, second);

		QVERIFY(!first.contains("mixerchannel"));
		QVERIFY(first.indexOf("a=\"1\"") < first.indexOf("b=\"2\""));
		// Repeated elements keep their relative order (track semantics rely on it).
		QVERIFY(first.indexOf("name=\"B\"") < first.indexOf("name=\"A\""));
	}
};

QTEST_GUILESS_MAIN(HeadlessUtilsTest)

#include "HeadlessUtilsTest.moc"
