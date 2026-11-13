/*
 * HeadlessWav.cpp - WAV metadata helpers for deterministic headless renders
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

#include "HeadlessWav.h"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <functional>

namespace lmms
{

namespace
{

constexpr qint64 ChunkHeaderSize = 8;
constexpr qint64 CopyBufferSize = 64 * 1024;

//! Chunks that carry no audio data and may change between otherwise identical
//! renders (PEAK in particular stores a creation timestamp).
bool isMetadataChunk(const QByteArray& id)
{
	return id == QByteArrayLiteral("PEAK")
		|| id == QByteArrayLiteral("LIST")
		|| id == QByteArrayLiteral("id3 ")
		|| id == QByteArrayLiteral("ID3 ")
		|| id == QByteArrayLiteral("bext")
		|| id == QByteArrayLiteral("fact");
}


void setError(QString* error, const QString& message)
{
	if (error) { *error = message; }
}


bool copyBytes(QFile& in, QSaveFile& out, qint64 count)
{
	char buffer[CopyBufferSize];
	while (count > 0)
	{
		const qint64 n = in.read(buffer, std::min<qint64>(count, CopyBufferSize));
		if (n <= 0) { return false; }
		if (out.write(buffer, n) != n) { return false; }
		count -= n;
	}
	return true;
}


//! Walk the RIFF chunk list of @a in and call @a handler for every chunk.
//! The handler receives the chunk id, its payload size and the absolute
//! payload offset. Returns false and fills @a error if the file is malformed.
bool forEachChunk(QFile& in, qint64 fileSize, const QString& path,
	const std::function<bool(const QByteArray&, quint32, qint64)>& handler,
	QString* error)
{
	qint64 offset = 12;
	while (offset + ChunkHeaderSize <= fileSize)
	{
		if (!in.seek(offset))
		{
			setError(error, QStringLiteral("could not seek in WAV file: %1").arg(path));
			return false;
		}

		const QByteArray chunkHeader = in.read(ChunkHeaderSize);
		if (chunkHeader.size() != ChunkHeaderSize)
		{
			setError(error, QStringLiteral("truncated WAV chunk header: %1").arg(path));
			return false;
		}

		const QByteArray id = chunkHeader.left(4);
		const quint32 size = qFromLittleEndian<quint32>(
			reinterpret_cast<const uchar*>(chunkHeader.constData() + 4));

		const qint64 dataEnd = offset + ChunkHeaderSize + size;
		if (dataEnd > fileSize)
		{
			setError(error, QStringLiteral("truncated WAV chunk \"%1\": %2")
				.arg(QString::fromLatin1(id.constData(), id.size()), path));
			return false;
		}

		const qint64 paddedEnd = dataEnd + (size & 1u);
		const bool hasPadding = paddedEnd <= fileSize;

		if (!handler(id, size, offset + ChunkHeaderSize)) { return false; }

		offset = (size & 1u) && hasPadding ? paddedEnd : dataEnd;
	}

	if (offset != fileSize)
	{
		setError(error, QStringLiteral("unexpected trailing data in WAV file: %1").arg(path));
		return false;
	}

	return true;
}

} // namespace


bool stripWavMetadata(const QString& path, QString* error)
{
	QFile in(path);
	if (!in.open(QIODevice::ReadOnly))
	{
		setError(error, QStringLiteral("could not open WAV file for reading: %1").arg(path));
		return false;
	}

	const qint64 fileSize = in.size();
	QByteArray header = in.read(12);
	if (header.size() != 12 || header.left(4) != QByteArrayLiteral("RIFF")
		|| header.mid(8, 4) != QByteArrayLiteral("WAVE"))
	{
		setError(error, QStringLiteral("not a RIFF/WAVE file: %1").arg(path));
		return false;
	}

	// First pass: validate the chunk list and compute the new RIFF size, so
	// that the header can be written in one go without seeking on QSaveFile.
	quint32 riffPayloadSize = 4; // "WAVE"
	const bool scanned = forEachChunk(in, fileSize, path,
		[&riffPayloadSize](const QByteArray& id, quint32 size, qint64) {
			if (!isMetadataChunk(id))
			{
				riffPayloadSize += static_cast<quint32>(ChunkHeaderSize + size + (size & 1u));
			}
			return true;
		}, error);
	if (!scanned) { return false; }

	QSaveFile out(path);
	if (!out.open(QIODevice::WriteOnly))
	{
		setError(error, QStringLiteral("could not open WAV file for writing: %1").arg(path));
		return false;
	}

	char riffHeader[12];
	std::memcpy(riffHeader, "RIFF", 4);
	qToLittleEndian<quint32>(riffPayloadSize, reinterpret_cast<uchar*>(riffHeader + 4));
	std::memcpy(riffHeader + 8, "WAVE", 4);
	if (out.write(riffHeader, sizeof(riffHeader)) != static_cast<qint64>(sizeof(riffHeader)))
	{
		out.cancelWriting();
		setError(error, QStringLiteral("could not write WAV header: %1").arg(path));
		return false;
	}

	// Second pass: copy all chunks except the metadata ones. The fmt and data
	// chunks (and therefore the PCM payload) are preserved byte for byte.
	const bool copied = forEachChunk(in, fileSize, path,
		[&in, &out, &path, error](const QByteArray& id, quint32 size, qint64 dataOffset) {
			if (isMetadataChunk(id)) { return true; }

			char chunkHeader[ChunkHeaderSize];
			std::memcpy(chunkHeader, id.constData(), 4);
			qToLittleEndian<quint32>(size, reinterpret_cast<uchar*>(chunkHeader + 4));
			if (out.write(chunkHeader, ChunkHeaderSize) != ChunkHeaderSize
				|| !in.seek(dataOffset) || !copyBytes(in, out, size))
			{
				setError(error, QStringLiteral("could not copy WAV chunk \"%1\": %2")
					.arg(QString::fromLatin1(id.constData(), id.size()), path));
				return false;
			}
			if (size & 1u)
			{
				// RIFF chunks are padded to an even boundary.
				const char pad = 0;
				if (out.write(&pad, 1) != 1)
				{
					setError(error, QStringLiteral("could not write WAV chunk padding: %1").arg(path));
					return false;
				}
			}
			return true;
		}, error);
	if (!copied)
	{
		out.cancelWriting();
		return false;
	}

	if (!out.commit())
	{
		setError(error, QStringLiteral("could not finalize stripped WAV file: %1").arg(path));
		return false;
	}

	return true;
}

} // namespace lmms
