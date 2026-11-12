/*
 * main.cpp - just main.cpp which is starting up app...
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2012-2013 Paul Giblock    <p/at/pgiblock.net>
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

#include "lmmsconfig.h"
#include "lmmsversion.h"
#include "versioninfo.h"

#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTimer>
#include <QTranslator>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTextStream>

#ifdef LMMS_BUILD_WIN32
#include <windows.h>
#endif

#ifdef LMMS_HAVE_PROCESS_H
#include <process.h>
#endif

#ifdef LMMS_HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef LMMS_HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <csignal>  // To register the signal handler

#include "MainApplication.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "NotePlayHandle.h"
#include "embed.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "Hardware.h"
#include "ImportFilter.h"
#include "MainWindow.h"
#include "MixHelpers.h"
#include "OutputSettings.h"
#include "ProjectRenderer.h"
#include "RenderManager.h"
#include "Song.h"

#include "AudioBusHandle.h"
#include "DummyEffect.h"
#include "Effect.h"
#include "EffectChain.h"
#include "HeadlessActions.h"
#include "HeadlessUtils.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "PatternStore.h"

#ifdef LMMS_DEBUG_FPE
#include <fenv.h> // For feenableexcept
#include <execinfo.h> // For backtrace and backtrace_symbols_fd
#include <unistd.h> // For STDERR_FILENO
#endif


#ifdef LMMS_DEBUG_FPE
void sigfpeHandler(int signum)
{

	// Get a back trace
	void *array[10];
	size_t size;

	// get void*'s for all entries on the stack
	size = backtrace(array, 10);

	backtrace_symbols_fd(array, size, STDERR_FILENO);

	// cleanup and close up stuff here
	// terminate program

	exit(signum);
}
#endif

static inline QString baseName( const QString & file )
{
	return QFileInfo( file ).absolutePath() + "/" +
			QFileInfo( file ).completeBaseName();
}


#ifdef LMMS_BUILD_WIN32
// Workaround for old MinGW
#ifdef __MINGW32__
extern "C" _CRTIMP errno_t __cdecl freopen_s(FILE** _File,
	const char *_Filename, const char *_Mode, FILE *_Stream);
#endif

// For qInstallMessageHandler
void consoleMessageHandler(QtMsgType type,
	const QMessageLogContext &context, const QString &msg)
{
    QByteArray localMsg = msg.toLocal8Bit();
    fprintf(stderr, "%s\n", localMsg.constData());
}
#endif // LMMS_BUILD_WIN32


inline void loadTranslation( const QString & tname,
	const QString & dir = lmms::ConfigManager::inst()->localeDir() )
{
	auto t = new QTranslator(QCoreApplication::instance());
	QString name = tname + ".qm";

	if (t->load(name, dir))
	{
		QCoreApplication::instance()->installTranslator(t);
	}
}




void printVersion( char *executableName )
{
	printf("LMMS %s\n(%s %s, Qt %s, %s)\n\n"
		"Build options:\n%s\n\n"
		"Copyright (c) %s\n\n"
		"This program is free software; you can redistribute it and/or\n"
		"modify it under the terms of the GNU General Public\n"
		"License as published by the Free Software Foundation; either\n"
		"version 2 of the License, or (at your option) any later version.\n\n"
		"Try \"%s --help\" for more information.\n\n", LMMS_VERSION,
		LMMS_BUILDCONF_PLATFORM, LMMS_BUILDCONF_MACHINE, QT_VERSION_STR, LMMS_BUILDCONF_COMPILER_VERSION, LMMS_BUILD_OPTIONS,
		LMMS_PROJECT_COPYRIGHT, executableName);
}




void printHelp()
{
	printf( "LMMS %s\n"
		"Copyright (c) %s\n\n"
		"Usage: lmms [global options...] [<action> [action parameters...]]\n\n"
		"Actions:\n"
		"  <no action> [options...] [<project>]  Start LMMS in normal GUI mode\n"
		"  dump <in>                             Dump XML of compressed file <in>\n"
		"  compress <in>                         Compress file <in>\n"
		"  render <project> [options...]         Render given project file\n"
		"  rendertracks <project> [options...]   Render each track to a different file\n"
		"  upgrade <in> [out]                    Upgrade file <in> and save as <out>\n"
		"                                        Standard out is used if no output file\n"
		"                                        is specified\n"
		"  makebundle <in> [out]                 Make a project bundle from the project\n"
		"                                        file <in> saving the resulting bundle\n"
		"                                        as <out>\n"
		"  save <in> [out] [--canonical] [--minimal]\n"
		"                                        Load <in> and save it again. Output goes\n"
		"                                        to <out> or stdout if <out> is omitted.\n"
		"  script <project> <file.json> [--out <out.mmp>] [--json]\n"
		"                                        Apply a JSON instruction script and save\n"
		"  normalize-basenote <project> [--out <out.mmp>] [--in-place] [--dry-run] [--json]\n"
		"                                        Migrate legacy basenote 57 tracks to 69\n"
		"  automation-add <project> --track <name> [--effect <index>]\n"
		"      --param <name> --point <tick>:<value> --out <out.mmp>\n"
		"                                        Add automation for a real model\n"
		"  --plugins-json [--type instrument|effect|ladspa|vst|all]\n"
		"                                        Print plugin parameter metadata as JSON\n"
		"  --model-ids <project>                 Print automation model id mapping as JSON\n"
		"\nGlobal options:\n"
		"      --allowroot                Bypass root user startup check (use with\n"
		"          caution).\n"
		"  -c, --config <configfile>      Get the configuration from <configfile>\n"
		"  -h, --help                     Show this usage information and exit.\n"
		"  -v, --version                  Show version information and exit.\n"
		"\nOptions if no action is given:\n"
		"      --geometry <geometry>      Specify the size and position of\n"
		"          the main window\n"
		"          geometry is <xsizexysize+xoffset+yoffsety>.\n"
		"      --import <in> [-e]         Import MIDI or Hydrogen file <in>.\n"
		"          If -e is specified lmms exits after importing the file.\n"
		"\nOptions for \"render\" and \"rendertracks\":\n"
		"  -a, --float                    Use 32bit float bit depth\n"
		"  -b, --bitrate <bitrate>        Specify output bitrate in KBit/s\n"
		"          Default: 160.\n"
		"  -f, --format <format>         Specify format of render-output where\n"
		"          Format is either 'wav', 'flac', 'ogg' or 'mp3'.\n"
		"  -l, --loop                     Render as a loop\n"
		"  -m, --mode                     Stereo mode used for MP3 export\n"
		"          Possible values: s, j, m\n"
		"            s: Stereo\n"
		"            j: Joint Stereo\n"
		"            m: Mono\n"
		"          Default: j\n"
		"  -o, --output <path>            Render into <path>\n"
		"          For \"render\", provide a file path\n"
		"          For \"rendertracks\", provide a directory path\n"
		"          If not specified, render will overwrite the input file\n"
		"          For \"rendertracks\", this might be required\n"
		"  -p, --profile <out>            Dump profiling information to file <out>\n"
		"  -s, --samplerate <samplerate>  Specify output samplerate in Hz\n"
		"          Range: 44100 (default) to 192000\n"
		"          Possible values: 1, 2, 4, 8\n"
		"          Default: 2\n"
		"      --from <tick|\"N bars\"|\"N beats\">\n"
		"                                  Start of the rendered region\n"
		"      --to <tick|\"N bars\"|\"N beats\">\n"
		"                                  End of the rendered region\n"
		"      --tail <seconds>            Append silence after the content\n"
		"      --normalize <dBFS>          Normalize float PCM before encoding\n"
		"      --json                      Print render statistics/errors as JSON\n\n",
		LMMS_VERSION, LMMS_PROJECT_COPYRIGHT );
}




void fileCheck( QString &file )
{
	QFileInfo fileToCheck( file );

	if( !fileToCheck.size() )
	{
		printf( "The file %s does not have any content.\n",
				file.toUtf8().constData() );
		exit( EXIT_FAILURE );
	}
	else if( fileToCheck.isDir() )
	{
		printf( "%s is a directory.\n",
				file.toUtf8().constData() );
		exit( EXIT_FAILURE );
	}
}

int usageError(const QString& message)
{
	qCritical().noquote() << QString("\n%1.\n\nTry \"%2 --help\" for more information.\n\n")
				   .arg( message ).arg( qApp->arguments()[0] );
	return EXIT_FAILURE;
}

int noInputFileError()
{
	return usageError( "No input file specified" );
}


void printJsonError( const QString & code, const QString & message )
{
	QJsonObject error;
	error["code"] = code;
	error["message"] = message;

	QJsonObject root;
	root["ok"] = false;
	root["error"] = error;

	fprintf( stdout, "%s\n", QJsonDocument( root ).toJson( QJsonDocument::Compact ).constData() );
	fflush( stdout );
}


int renderOptionError( bool json, const QString & code, const QString & message )
{
	if( json )
	{
		printJsonError( code, message );
		return EXIT_FAILURE;
	}
	return usageError( message );
}


QStringList collectResourceWarnings()
{
	using namespace lmms;

	QStringList warnings;

	Song * song = Engine::getSong();
	if( song->hasErrors() )
	{
		warnings.append( song->errorSummary().split( '\n', Qt::SkipEmptyParts ) );
	}

	auto scanContainer = [&warnings]( const TrackContainer::TrackList & tracks ) {
		for( Track * track : tracks )
		{
			if( auto instrumentTrack = dynamic_cast<InstrumentTrack *>( track ) )
			{
				if( instrumentTrack->instrument() != nullptr &&
					instrumentTrack->instrument()->nodeName() == "dummyinstrument" )
				{
					warnings << QString( "plugin-missing: track \"%1\" uses an unavailable instrument" )
						.arg( track->name() );
				}
			}

			AudioBusHandle * bus = nullptr;
			if( auto instrumentTrack = dynamic_cast<InstrumentTrack *>( track ) )
			{
				bus = instrumentTrack->audioBusHandle();
			}
			if( bus != nullptr )
			{
				for( Effect * effect : bus->effects()->findChildren<Effect *>() )
				{
					if( dynamic_cast<DummyEffect *>( effect ) != nullptr )
					{
						warnings << QString( "plugin-missing: track \"%1\" uses an unavailable effect" )
							.arg( track->name() );
					}
				}
			}
		}
	};

	scanContainer( song->tracks() );
	scanContainer( Engine::patternStore()->tracks() );

	warnings.removeDuplicates();
	return warnings;
}


int main( int argc, char * * argv )
{
	using namespace lmms;

	bool coreOnly = false;
	bool fullscreen = true;
	bool exitAfterImport = false;
	bool allowRoot = false;
	bool renderLoop = false;
	bool renderTracks = false;
	QString fileToLoad, fileToImport, renderOut, profilerOutputFile, configFile;

	// Headless actions implemented on top of the model layer.
	QString headlessAction;
	QString saveIn, saveOut, scriptFile, scriptOut, pluginsType;
	bool saveCanonical = false;
	bool saveMinimal = false;
	bool scriptJson = false;
	QString actionProject, actionOut;
	bool normalizeInPlace = false;
	bool normalizeDryRun = false;
	bool actionJson = false;
	QString automationTrackName;
	int automationEffect = -1;
	QString automationParam;
	QList<lmms::headless::AutomationPoint> automationPoints;

	// render/rendertracks addition options
	bool jsonRender = false;
	QString fromSpec, toSpec;
	double renderTail = 0.0;
	bool renderNormalize = false;
	double renderNormalizeDb = 0.0;

	// first of two command-line parsing stages
	for (int i = 1; i < argc; ++i)
	{
		QString arg = argv[i];

		if (arg == "--help" || arg == "-h")
		{
			printHelp();
			return EXIT_SUCCESS;
		}
		else if (arg == "--version" || arg == "-v")
		{
			printVersion(argv[0]);
			return EXIT_SUCCESS;
		}
		else if (arg == "render" || arg == "--render" || arg == "-r" )
		{
			coreOnly = true;
		}
		else if (arg == "rendertracks" || arg == "--rendertracks")
		{
			coreOnly = true;
			renderTracks = true;
		}
		else if (arg == "save" || arg == "script" || arg == "--plugins-json" ||
			arg == "--model-ids" || arg == "normalize-basenote" || arg == "automation-add")
		{
			coreOnly = true;
		}
		else if (arg == "--json")
		{
			jsonRender = true;
		}
		else if (arg == "--allowroot")
		{
			allowRoot = true;
		}
		else if (arg == "--geometry" || arg == "-geometry")
		{
			if (arg == "--geometry") { argv[i]++; } // Delete the first "-" so Qt recognize the option
			// option -geometry is filtered by Qt later,
			// so we need to check its presence now to
			// determine, if the application should run in
			// fullscreen mode (default, no -geometry given).
			fullscreen = false;
		}
	}

#ifdef LMMS_DEBUG_FPE
	// Enable exceptions for certain floating point results
	// FE_UNDERFLOW is disabled for the time being
	feenableexcept( FE_INVALID   |
			FE_DIVBYZERO |
			FE_OVERFLOW  /*|
			FE_UNDERFLOW*/);

	// Install the trap handler
	// register signal SIGFPE and signal handler
	signal(SIGFPE, sigfpeHandler);
#endif
	signal(SIGINT, gui::GuiApplication::sigintHandler);

#ifdef LMMS_BUILD_WIN32
	// Don't touch redirected streams here
	// GetStdHandle should be called before AttachConsole
	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
	FILE *fIn, *fOut, *fErr;
	// Enable console output if available
	if (AttachConsole(ATTACH_PARENT_PROCESS))
	{
		if (!hStdIn)
		{
			freopen_s(&fIn, "CONIN$", "r", stdin);
		}
		if (!hStdOut)
		{
			freopen_s(&fOut, "CONOUT$", "w", stdout);
		}
		if (!hStdErr)
		{
			freopen_s(&fErr, "CONOUT$", "w", stderr);
		}
	}
	// Make Qt's debug message handlers work
	qInstallMessageHandler(consoleMessageHandler);
#endif

#if defined(LMMS_HAVE_SYS_PRCTL_H) && defined(PR_SET_CHILD_SUBREAPER)
	// Set the "child subreaper" attribute so that plugin child processes remain as lmms'
	// children even when some wrapper process exits, as it may happen with wine
	if (prctl(PR_SET_CHILD_SUBREAPER, 1))
	{
		perror("prctl(PR_SET_CHILD_SUBREAPER)");
	}
#endif

	// initialize memory managers
	NotePlayHandleManager::init();

	// initialize RNG
	srand( getpid() + time( 0 ) );

	disableDenormals();

#if !defined(LMMS_BUILD_WIN32) && !defined(LMMS_BUILD_HAIKU)
	if ( ( getuid() == 0 || geteuid() == 0 ) && !allowRoot )
	{
		printf( "LMMS cannot be run as root.\nUse \"--allowroot\" to override.\n\n" );
		return EXIT_FAILURE;
	}
#endif

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
	// High-DPI scaling is always enabled in Qt >= 6.0
	QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

	QCoreApplication * app = coreOnly ?
			new QCoreApplication( argc, argv ) :
					new gui::MainApplication(argc, argv);

	OutputSettings os(44100, 160, OutputSettings::BitDepth::Depth16Bit, OutputSettings::StereoMode::JointStereo);
	ProjectRenderer::ExportFileFormat eff = ProjectRenderer::ExportFileFormat::Wave;

	// second of two command-line parsing stages
	for( int i = 1; i < argc; ++i )
	{
		QString arg = argv[i];

		if (arg == "upgrade" || arg == "--upgrade" || arg  == "-u")
		{
			++i;

			if( i == argc )
			{
				return noInputFileError();
			}


			DataFile dataFile( QString::fromLocal8Bit( argv[i] ) );

			if( argc > i+1 ) // output file specified
			{
				dataFile.writeFile( QString::fromLocal8Bit( argv[i+1] ) );
			}
			else // no output file specified; use stdout
			{
				QTextStream ts( stdout );
				dataFile.write( ts );
				fflush( stdout );
			}

			return EXIT_SUCCESS;
		}
		else if (arg == "makebundle")
		{
			++i;

			if (i == argc)
			{
				return noInputFileError();
			}

			DataFile dataFile(QString::fromLocal8Bit(argv[i]));

			if (argc > i+1) // Project bundle file name given
			{
				printf("Making bundle\n");
				dataFile.writeFile(QString::fromLocal8Bit(argv[i+1]), true);
				return EXIT_SUCCESS;
			}
			else
			{
				return usageError("No project bundle name given");
			}
		}
		else if( arg == "--allowroot" )
		{
			// Ignore, processed earlier
#ifdef LMMS_BUILD_WIN32
			if( allowRoot )
			{
				printf( "\nOption \"--allowroot\" will be ignored on this platform.\n\n" );
			}
#endif

		}
		else if( arg == "dump" || arg == "--dump" || arg  == "-d" )
		{
			++i;

			if( i == argc )
			{
				return noInputFileError();
			}


			QFile f( QString::fromLocal8Bit( argv[i] ) );
			f.open( QIODevice::ReadOnly );
			QString d = qUncompress( f.readAll() );
			printf( "%s\n", d.toUtf8().constData() );

			return EXIT_SUCCESS;
		}
		else if( arg == "compress" || arg == "--compress" )
		{
			++i;

			if( i == argc )
			{
				return noInputFileError();
			}

			QFile f( QString::fromLocal8Bit( argv[i] ) );
			f.open( QIODevice::ReadOnly );
			QByteArray d = qCompress( f.readAll() ) ;
			fwrite( d.constData(), sizeof(char), d.size(), stdout );

			return EXIT_SUCCESS;
		}
		else if( arg == "save" )
		{
			++i;
			if( i == argc ) { return noInputFileError(); }
			saveIn = QString::fromLocal8Bit( argv[i] );

			if( i + 1 < argc && argv[i + 1][0] != '-' )
			{
				saveOut = QString::fromLocal8Bit( argv[++i] );
			}

			while( i + 1 < argc )
			{
				const QString flag = QString::fromLocal8Bit( argv[i + 1] );
				if( flag == "--canonical" ) { saveCanonical = true; ++i; }
				else if( flag == "--minimal" ) { saveMinimal = true; ++i; }
				else { return usageError( QString( "Invalid option %1" ).arg( flag ) ); }
			}

			headlessAction = "save";
			break;
		}
		else if( arg == "script" )
		{
			++i;
			if( i == argc ) { return noInputFileError(); }
			actionProject = QString::fromLocal8Bit( argv[i] );

			++i;
			if( i == argc ) { return usageError( "No script file specified" ); }
			scriptFile = QString::fromLocal8Bit( argv[i] );

			while( i + 1 < argc )
			{
				const QString flag = QString::fromLocal8Bit( argv[i + 1] );
				if( flag == "--out" && i + 2 < argc )
				{
					scriptOut = QString::fromLocal8Bit( argv[i + 2] );
					i += 2;
				}
				else if( flag == "--json" ) { scriptJson = true; ++i; }
				else { return usageError( QString( "Invalid option %1" ).arg( flag ) ); }
			}

			headlessAction = "script";
			break;
		}
		else if( arg == "--plugins-json" )
		{
			pluginsType = "all";
			while( i + 1 < argc )
			{
				const QString flag = QString::fromLocal8Bit( argv[i + 1] );
				if( flag == "--type" && i + 2 < argc )
				{
					pluginsType = QString::fromLocal8Bit( argv[i + 2] );
					i += 2;
				}
				else { return usageError( QString( "Invalid option %1" ).arg( flag ) ); }
			}
			headlessAction = "plugins-json";
			break;
		}
		else if( arg == "--model-ids" )
		{
			++i;
			if( i == argc ) { return noInputFileError(); }
			actionProject = QString::fromLocal8Bit( argv[i] );
			headlessAction = "model-ids";
			break;
		}
		else if( arg == "normalize-basenote" )
		{
			++i;
			if( i == argc ) { return noInputFileError(); }
			actionProject = QString::fromLocal8Bit( argv[i] );

			while( i + 1 < argc )
			{
				const QString flag = QString::fromLocal8Bit( argv[i + 1] );
				if( flag == "--out" && i + 2 < argc )
				{
					actionOut = QString::fromLocal8Bit( argv[i + 2] );
					i += 2;
				}
				else if( flag == "--in-place" ) { normalizeInPlace = true; ++i; }
				else if( flag == "--dry-run" ) { normalizeDryRun = true; ++i; }
				else if( flag == "--json" ) { actionJson = true; ++i; }
				else { return usageError( QString( "Invalid option %1" ).arg( flag ) ); }
			}

			headlessAction = "normalize-basenote";
			break;
		}
		else if( arg == "automation-add" )
		{
			++i;
			if( i == argc ) { return noInputFileError(); }
			actionProject = QString::fromLocal8Bit( argv[i] );

			while( i + 1 < argc )
			{
				const QString flag = QString::fromLocal8Bit( argv[i + 1] );
				if( flag == "--track" && i + 2 < argc )
				{
					automationTrackName = QString::fromLocal8Bit( argv[i + 2] );
					i += 2;
				}
				else if( flag == "--effect" && i + 2 < argc )
				{
					bool ok = false;
					automationEffect = QString( argv[i + 2] ).toInt( &ok );
					if( !ok ) { return usageError( "--effect expects an integer index" ); }
					i += 2;
				}
				else if( flag == "--param" && i + 2 < argc )
				{
					automationParam = QString::fromLocal8Bit( argv[i + 2] );
					i += 2;
				}
				else if( flag == "--point" && i + 2 < argc )
				{
					const QString point = QString::fromLocal8Bit( argv[i + 2] );
					const int colon = point.indexOf( ':' );
					if( colon <= 0 )
					{
						return usageError( "--point expects <tick>:<value>" );
					}
					bool okTick = false;
					bool okValue = false;
					const int tick = point.left( colon ).toInt( &okTick );
					const double value = point.mid( colon + 1 ).toDouble( &okValue );
					if( !okTick || !okValue )
					{
						return usageError( "--point expects <tick>:<value>" );
					}
					automationPoints.append( { tick, value } );
					i += 2;
				}
				else if( flag == "--out" && i + 2 < argc )
				{
					actionOut = QString::fromLocal8Bit( argv[i + 2] );
					i += 2;
				}
				else { return usageError( QString( "Invalid option %1" ).arg( flag ) ); }
			}

			headlessAction = "automation-add";
			break;
		}
		else if( arg == "render" || arg == "--render" || arg == "-r" ||
			arg == "rendertracks" || arg == "--rendertracks" )
		{
			++i;

			if( i == argc )
			{
				return noInputFileError();
			}


			fileToLoad = QString::fromLocal8Bit( argv[i] );
			renderOut = fileToLoad;
		}
		else if( arg == "--loop" || arg == "-l" )
		{
			renderLoop = true;
		}
		else if( arg == "--output" || arg == "-o" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No output file specified" );
			}


			renderOut = QString::fromLocal8Bit( argv[i] );
		}
		else if( arg == "--format" || arg == "-f" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No output format specified" );
			}


			const QString ext = QString( argv[i] );

			if( ext == "wav" )
			{
				eff = ProjectRenderer::ExportFileFormat::Wave;
			}
#ifdef LMMS_HAVE_OGGVORBIS
			else if( ext == "ogg" )
			{
				eff = ProjectRenderer::ExportFileFormat::Ogg;
			}
#endif
#ifdef LMMS_HAVE_MP3LAME
			else if( ext == "mp3" )
			{
				eff = ProjectRenderer::ExportFileFormat::MP3;
			}
#endif
			else if (ext == "flac")
			{
				eff = ProjectRenderer::ExportFileFormat::Flac;
			}
			else
			{
				return renderOptionError( jsonRender, "invalid-format",
					QString( "Invalid output format %1" ).arg( argv[i] ) );
			}
		}
		else if( arg == "--samplerate" || arg == "-s" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No samplerate specified" );
			}


			sample_rate_t sr = QString( argv[i] ).toUInt();
			if( sr >= 44100 && sr <= 192000 )
			{
				os.setSampleRate(sr);
			}
			else
			{
				return usageError( QString( "Invalid samplerate %1" ).arg( argv[i] ) );
			}
		}
		else if( arg == "--bitrate" || arg == "-b" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No bitrate specified" );
			}


			int br = QString( argv[i] ).toUInt();

			if( br >= 64 && br <= 384 )
			{
				os.setBitrate(br);
			}
			else
			{
				return usageError( QString( "Invalid bitrate %1" ).arg( argv[i] ) );
			}
		}
		else if( arg == "--mode" || arg == "-m" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No stereo mode specified" );
			}

			QString const mode( argv[i] );

			if( mode == "s" )
			{
				os.setStereoMode(OutputSettings::StereoMode::Stereo);
			}
			else if( mode == "j" )
			{
				os.setStereoMode(OutputSettings::StereoMode::JointStereo);
			}
			else if( mode == "m" )
			{
				os.setStereoMode(OutputSettings::StereoMode::Mono);
			}
			else
			{
				return usageError( QString( "Invalid stereo mode %1" ).arg( argv[i] ) );
			}
		}
		else if( arg =="--float" || arg == "-a" )
		{
			os.setBitDepth(OutputSettings::BitDepth::Depth32Bit);
		}
		else if( arg == "--import" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No file specified for importing" );
			}

			fileToImport = QString::fromLocal8Bit( argv[i] );

			// exit after import? (only for debugging)
			if( QString( argv[i + 1] ) == "-e" )
			{
				exitAfterImport = true;
				++i;
			}
		}
		else if( arg == "--profile" || arg == "-p" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No profile specified" );
			}


			profilerOutputFile = QString::fromLocal8Bit( argv[i] );
		}
		else if( arg == "--config" || arg == "-c" )
		{
			++i;

			if( i == argc )
			{
				return usageError( "No configuration file specified" );
			}

			configFile = QString::fromLocal8Bit( argv[i] );
		}
		else if( arg == "--json" )
		{
			jsonRender = true;
		}
		else if( arg == "--from" )
		{
			++i;
			if( i == argc )
			{
				return renderOptionError( jsonRender, "invalid-argument", "No start position specified" );
			}
			fromSpec = QString::fromLocal8Bit( argv[i] );
		}
		else if( arg == "--to" )
		{
			++i;
			if( i == argc )
			{
				return renderOptionError( jsonRender, "invalid-argument", "No end position specified" );
			}
			toSpec = QString::fromLocal8Bit( argv[i] );
		}
		else if( arg == "--tail" )
		{
			++i;
			if( i == argc )
			{
				return renderOptionError( jsonRender, "invalid-argument", "No tail length specified" );
			}
			bool ok = false;
			renderTail = QString( argv[i] ).toDouble( &ok );
			if( !ok || renderTail < 0.0 )
			{
				return renderOptionError( jsonRender, "invalid-argument",
					QString( "Invalid tail length %1" ).arg( argv[i] ) );
			}
		}
		else if( arg == "--normalize" )
		{
			++i;
			if( i == argc )
			{
				return renderOptionError( jsonRender, "invalid-argument", "No normalization level specified" );
			}
			bool ok = false;
			renderNormalizeDb = QString( argv[i] ).toDouble( &ok );
			if( !ok )
			{
				return renderOptionError( jsonRender, "invalid-argument",
					QString( "Invalid normalization level %1" ).arg( argv[i] ) );
			}
			renderNormalize = true;
		}
		else
		{
			if( argv[i][0] == '-' )
			{
				return usageError( QString( "Invalid option %1" ).arg( argv[i] ) );
			}
			fileToLoad = QString::fromLocal8Bit( argv[i] );
		}
	}

	// Validate the requested output extension before loading the project so
	// that a typo is reported as an invalid format instead of being silently
	// replaced by the default WAV encoder.
	if( !renderOut.isEmpty() && !renderTracks )
	{
		const QString suffix = QFileInfo( renderOut ).suffix();
		if( !suffix.isEmpty() && !ProjectRenderer::isSupportedExtension( suffix ) )
		{
			return renderOptionError( jsonRender, "invalid-format",
				QString( "Unsupported output format \".%1\"" ).arg( suffix ) );
		}
	}

	// Test file argument before continuing
	if( !fileToLoad.isEmpty() )
	{
		if( jsonRender && !renderOut.isEmpty() && !QFileInfo::exists( fileToLoad ) )
		{
			printJsonError( "project-not-found",
				QString( "Project file not found: %1" ).arg( fileToLoad ) );
			return EXIT_FAILURE;
		}
		fileCheck( fileToLoad );
	}
	else if( !fileToImport.isEmpty() )
	{
		fileCheck( fileToImport );
	}

	ConfigManager::inst()->loadConfigFile(configFile);

	// set language
	QString pos = ConfigManager::inst()->value( "app", "language" );
	if( pos.isEmpty() )
	{
		pos = QLocale::system().name().left( 2 );
	}

	// load actual translation for LMMS
	loadTranslation( pos );

	// load translation for Qt-widgets/-dialogs
#ifdef QT_TRANSLATIONS_DIR
	// load from the original path first
	loadTranslation(QString("qt_") + pos, QT_TRANSLATIONS_DIR);
#endif
	// override it with bundled/custom one, if exists
	loadTranslation(QString("qt_") + pos, ConfigManager::inst()->localeDir());

#if _POSIX_C_SOURCE >= 1 || _XOPEN_SOURCE || _POSIX_SOURCE
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_SIGINFO;
	if ( sigemptyset( &sa.sa_mask ) )
	{
		fprintf( stderr, "Signal initialization failed.\n" );
	}
	if ( sigaction( SIGPIPE, &sa, nullptr ) )
	{
		fprintf( stderr, "Signal initialization failed.\n" );
	}
#endif

	if( !headlessAction.isEmpty() )
	{
		Engine::init( true );

		int ret = EXIT_SUCCESS;
		if( headlessAction == "save" )
		{
			ret = lmms::headless::runSaveAction( saveIn, saveOut, saveCanonical, saveMinimal );
		}
		else if( headlessAction == "script" )
		{
			ret = lmms::headless::runScriptAction( actionProject, scriptFile, scriptOut, scriptJson );
		}
		else if( headlessAction == "plugins-json" )
		{
			ret = lmms::headless::runPluginsJsonAction( pluginsType );
		}
		else if( headlessAction == "normalize-basenote" )
		{
			ret = lmms::headless::runNormalizeBaseNoteAction(
				actionProject, actionOut, normalizeInPlace, normalizeDryRun, actionJson );
		}
		else if( headlessAction == "model-ids" )
		{
			ret = lmms::headless::runModelIdsAction( actionProject );
		}
		else if( headlessAction == "automation-add" )
		{
			ret = lmms::headless::runAutomationAddAction(
				actionProject, automationTrackName, automationEffect, automationParam,
				automationPoints, actionOut );
		}

		Engine::destroy();
		delete app;
		return ret;
	}

	bool destroyEngine = false;
	RenderManager * renderManager = nullptr;

	// if we have an output file for rendering, just render the song
	// without starting the GUI
	if( !renderOut.isEmpty() )
	{
		Engine::init( true );
		destroyEngine = true;

		if( !jsonRender ) { printf( "Loading project...\n" ); }
		Engine::getSong()->loadProject( fileToLoad );
		if( Engine::getSong()->isEmpty() )
		{
			if( jsonRender )
			{
				printJsonError( "empty-project",
					QString( "The project %1 is empty" ).arg( fileToLoad ) );
				Engine::destroy();
				delete app;
				return EXIT_FAILURE;
			}
			printf("The project %s is empty, aborting!\n", fileToLoad.toUtf8().constData() );
			exit( EXIT_FAILURE );
		}
		if( !jsonRender ) { printf( "Done\n" ); }

		Engine::getSong()->setExportLoop( renderLoop );

		// Native export range. Tempo automation is integrated by the engine
		// while rendering, so tick positions are exact even with tempo ramps.
		if( !fromSpec.isEmpty() || !toSpec.isEmpty() )
		{
			int ticksPerBar = Engine::getSong()->ticksPerBar();
			int beatsPerBar = Engine::getSong()->getTimeSigModel().getNumerator();
			int start = 0;
			int end = -1;
			QString timeError;

			if( !fromSpec.isEmpty() &&
				!lmms::headless::parseTimeSpec( fromSpec, ticksPerBar, beatsPerBar, &start, &timeError ) )
			{
				if( jsonRender )
				{
					printJsonError( "invalid-argument", timeError );
					Engine::destroy();
					delete app;
					return EXIT_FAILURE;
				}
				return usageError( timeError );
			}
			if( !toSpec.isEmpty() &&
				!lmms::headless::parseTimeSpec( toSpec, ticksPerBar, beatsPerBar, &end, &timeError ) )
			{
				if( jsonRender )
				{
					printJsonError( "invalid-argument", timeError );
					Engine::destroy();
					delete app;
					return EXIT_FAILURE;
				}
				return usageError( timeError );
			}

			Engine::getSong()->setExportRange( start, end );
		}

		// when rendering multiple tracks, renderOut is a directory
		// otherwise, it is a file, so we need to append the file extension
		if ( !renderTracks )
		{
			renderOut = baseName( renderOut ) +
				ProjectRenderer::getFileExtensionFromFormat(eff);
		}

		RenderOptions renderOptions;
		renderOptions.json = jsonRender;
		renderOptions.tailSeconds = renderTail;
		renderOptions.normalize = renderNormalize;
		renderOptions.normalizeDb = renderNormalizeDb;
		renderOptions.warnings = collectResourceWarnings();

		// create renderer
		renderManager = new RenderManager(os, eff, renderOut, renderOptions);
		QCoreApplication::instance()->connect( renderManager,
				SIGNAL(finished()), SLOT(quit()));

		// timer for progress-updates
		auto t = new QTimer(renderManager);
		renderManager->connect( t, SIGNAL(timeout()),
				SLOT(updateConsoleProgress()));
		t->start( 200 );

		if( profilerOutputFile.isEmpty() == false )
		{
			Engine::audioEngine()->profiler().setOutputFile( profilerOutputFile );
		}

		// start now!
		if ( renderTracks )
		{
			renderManager->renderTracks();
		}
		else
		{
			renderManager->renderProject();
		}
	}
	else // otherwise, start the GUI
	{
		using namespace lmms::gui;

		new GuiApplication();

		// re-intialize RNG - shared libraries might have srand() or
		// srandom() calls in their init procedure
		srand( getpid() + time( 0 ) );

		// recover a file?
		QString recoveryFile = ConfigManager::inst()->recoveryFile();

		bool recoveryFilePresent = QFileInfo( recoveryFile ).exists() &&
				QFileInfo( recoveryFile ).isFile();
		bool autoSaveEnabled =
			ConfigManager::inst()->value( "ui", "enableautosave" ).toInt();
		if( recoveryFilePresent )
		{
			QMessageBox mb;
			mb.setWindowTitle( MainWindow::tr( "Project recovery" ) );
			mb.setText( QString(
				"<html>"
				"<p style=\"margin-left:6\">%1</p>"
				"<table cellpadding=\"3\">"
				"  <tr>"
				"    <td><b>%2</b></td>"
				"    <td>%3</td>"
				"  </tr>"
				"  <tr>"
				"    <td><b>%4</b></td>"
				"    <td>%5</td>"
				"  </tr>"
				"</table>"
				"</html>" ).arg(
				MainWindow::tr( "There is a recovery file present. "
					"It looks like the last session did not end "
					"properly or another instance of LMMS is "
					"already running. Do you want to recover the "
					"project of this session?" ),
				MainWindow::tr( "Recover" ),
				MainWindow::tr( "Recover the file. Please don't run "
					"multiple instances of LMMS when you do this." ),
				MainWindow::tr( "Discard" ),
				MainWindow::tr( "Launch a default session and delete "
					"the restored files. This is not reversible." )
							) );

			mb.setIcon( QMessageBox::Warning );
			mb.setWindowIcon( embed::getIconPixmap( "icon_small" ) );
			mb.setWindowFlags( Qt::WindowCloseButtonHint );

			// setting all buttons to the same roles allows us
			// to have a custom layout
			auto discard = mb.addButton(MainWindow::tr("Discard"), QMessageBox::AcceptRole);
			auto recover = mb.addButton(MainWindow::tr("Recover"), QMessageBox::AcceptRole);

			// have a hidden exit button
			auto exit = mb.addButton("", QMessageBox::RejectRole);
			exit->setVisible(false);

			// set icons
			recover->setIcon( embed::getIconPixmap( "recover" ) );
			discard->setIcon( embed::getIconPixmap( "discard" ) );

			mb.setDefaultButton( recover );
			mb.setEscapeButton( exit );

			mb.exec();
			if( mb.clickedButton() == discard )
			{
				getGUI()->mainWindow()->sessionCleanup();
			}
			else if( mb.clickedButton() == recover ) // Recover
			{
				fileToLoad = recoveryFile;
				getGUI()->mainWindow()->setSession( MainWindow::SessionState::Recover );
			}
			else // Exit
			{
				return EXIT_SUCCESS;
			}
		}

		// first show the Main Window and then try to load given file

		// [Settel] workaround: showMaximized() doesn't work with
		// FVWM2 unless the window is already visible -> show() first
		getGUI()->mainWindow()->show();
		if( fullscreen )
		{
			getGUI()->mainWindow()->showMaximized();
		}

		// Handle macOS-style FileOpen QEvents
		QString queuedFile = static_cast<MainApplication *>( app )->queuedFile();
		if ( !queuedFile.isEmpty() ) {
			fileToLoad = queuedFile;
		}

		if( !fileToLoad.isEmpty() )
		{
			if( fileToLoad == recoveryFile )
			{
				Engine::getSong()->createNewProjectFromTemplate( fileToLoad );
			}
			else
			{
				Engine::getSong()->loadProject( fileToLoad );
			}
		}
		else if( !fileToImport.isEmpty() )
		{
			ImportFilter::import( fileToImport, Engine::getSong() );
			if( exitAfterImport )
			{
				return EXIT_SUCCESS;
			}
		}
		// If enabled, open last project if there is one. Else, create
		// a new one.
		else if( ConfigManager::inst()->
				value( "app", "openlastproject" ).toInt() &&
			!ConfigManager::inst()->
				recentlyOpenedProjects().isEmpty() &&
				!recoveryFilePresent )
		{
			QString f = ConfigManager::inst()->
					recentlyOpenedProjects().first();
			QFileInfo recentFile( f );

			if ( recentFile.exists() &&
				recentFile.suffix().toLower() != "mpt" )
			{
				Engine::getSong()->loadProject( f );
			}
			else
			{
				Engine::getSong()->createNewProject();
			}
		}
		else
		{
			Engine::getSong()->createNewProject();
		}

		// Finally we start the auto save timer and also trigger the
		// autosave one time as recover.mmp is a signal to possible other
		// instances of LMMS.
		if( autoSaveEnabled )
		{
			gui::getGUI()->mainWindow()->autoSaveTimerReset();
		}
	}

	const int ret = app->exec();

	QString jsonReport;
	int jsonExitCode = ret;

	if( jsonRender && renderManager != nullptr )
	{
		RenderStats stats = renderManager->stats();

		for( const QString & warning : collectResourceWarnings() )
		{
			if( !stats.warnings.contains( warning ) ) { stats.warnings << warning; }
		}

		bool missingSample = false;
		bool missingPlugin = false;
		for( const QString & warning : stats.warnings )
		{
			if( warning.contains( "sample not found", Qt::CaseInsensitive ) )
			{
				missingSample = true;
			}
			if( warning.contains( "plugin-missing", Qt::CaseInsensitive ) )
			{
				missingPlugin = true;
			}
		}

		bool ok = stats.valid;
		QString errorCode;
		QString errorMessage;

		if( !ok )
		{
			errorCode = "render-failed";
			errorMessage = QString( "could not render %1" ).arg( renderOut );
		}
		else if( stats.silent && missingSample )
		{
			ok = false;
			errorCode = "sample-missing";
			errorMessage = "rendered audio is silent because samples could not be found";
		}
		else if( stats.silent && missingPlugin )
		{
			ok = false;
			errorCode = "plugin-missing";
			errorMessage = "rendered audio is silent because plugins could not be loaded";
		}

		QJsonObject root;
		if( ok )
		{
			QJsonArray warnings;
			for( const QString & warning : stats.warnings ) { warnings.append( warning ); }

			root["ok"] = true;
			root["file"] = stats.file;
			root["format"] = stats.format;
			root["sampleRate"] = stats.sampleRate;
			root["duration"] = stats.duration;
			root["peak"] = stats.peak;
			root["rms"] = stats.rms;
			root["silent"] = stats.silent;
			root["tracks"] = stats.tracks;
			root["warnings"] = warnings;
			jsonExitCode = 0;
		}
		else
		{
			QJsonObject error;
			error["code"] = errorCode;
			error["message"] = errorMessage;

			root["ok"] = false;
			root["error"] = error;
			if( !stats.warnings.isEmpty() )
			{
				QJsonArray warnings;
				for( const QString & warning : stats.warnings ) { warnings.append( warning ); }
				root["warnings"] = warnings;
			}
			jsonExitCode = EXIT_FAILURE;
		}

		jsonReport = QString::fromUtf8(
			QJsonDocument( root ).toJson( QJsonDocument::Compact ) ) + "\n";
	}

	delete app;

	if( destroyEngine )
	{
		Engine::destroy();
	}

	NotePlayHandleManager::free();

	if( !jsonReport.isEmpty() )
	{
		fprintf( stdout, "%s", jsonReport.toUtf8().constData() );
		fflush( stdout );
		return jsonExitCode;
	}

	// ProjectRenderer::updateConsoleProgress() doesn't return line after render
	if( coreOnly )
	{
		printf( "\n" );
	}

#ifdef LMMS_BUILD_WIN32
	// Cleanup console
	HWND hConsole = GetConsoleWindow();
	if (hConsole)
	{
		SendMessage(hConsole, WM_CHAR, (WPARAM)VK_RETURN, (LPARAM)0);
		FreeConsole();
	}
#endif

	return ret;
}
