// $Id$

/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Deadly Rooms of Death.
 *
 * The Initial Developer of the Original Code is
 * Caravel Software.
 * Portions created by the Initial Developer are Copyright (C) 2002, 2005 Caravel
 * Software. All Rights Reserved.
 *
 * Contributor(s):
 * Matt Schikore (schik), Mike Rimer (mrimer), Jamieson Cobleigh, Gerry Jo Jellestad (trick)
 *
 * ***** END LICENSE BLOCK ***** */

//#define STEAMBUILD_TSS_APP //uncomment to create a TSS app-specific build for Steam

#ifdef WIN32
#  include <windows.h> //Should be first include.
#  pragma warning(disable:4786)
#endif

#include "DrodFileDialogWidget.h"
#include "DrodFontManager.h"
#include "DrodBitmapManager.h"
#include "DrodScreen.h"
#include "DrodScreenManager.h"
#include "DrodSound.h"
#include "GameScreen.h"

#include <FrontEndLib/ImageWidget.h>

#include "../DRODLib/Db.h"
#include "../DRODLib/DbPlayers.h"
#include "../DRODLib/DbXML.h"
#include "../DRODLib/GameConstants.h"
#include "../DRODLib/NetInterface.h"
#include "../DRODLib/SettingsKeys.h"
#ifdef CARAVELBUILD
#	include "../CaravelNet/CaravelNetInterface.h"
#endif
#include "../Texts/MIDs.h"
#include <BackEndLib/Assert.h>
#include <BackEndLib/Exception.h>
#include <BackEndLib/Files.h>
#include <BackEndLib/Ports.h>
#include <BackEndLib/Date.h>
#include <BackEndLib/Internet.h>
#include <BackEndLib/Metadata.h>

#if defined(__linux__) || defined(__FreeBSD__)
#	include <BackEndLib/Dyn.h>
#endif

#ifdef STEAMBUILD
#	include <steam_api.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
#	include <sys/types.h>
#	include <sys/stat.h>
#	include <sys/time.h>
#	include <sys/resource.h>
#	include <unistd.h>
#	include <errno.h>
#endif

#if defined(__FreeBSD__)
#	include <kvm.h>
#	include <fcntl.h>
#	include <sys/sysctl.h>
#	include <limits.h>
#	include <signal.h>
#endif

#if defined(__APPLE__)
#	include <signal.h>

#   include <sys/sysctl.h>
#   include <sys/param.h>
#   include <sys/mount.h>
#   include <sys/types.h>

#   include <SDL_main.h>
#   include <SDL.h>
#endif

#include <fstream>
#include <string>
#include <vector>
#include <SDL_syswm.h>

using std::string;

//Vars used to protect against multiple instances of the app running at once.
#ifdef WIN32
//This value is used to set a window property of DROD so it will be distinguished from windows of
//other applications.  The value is arbitrary--in this case it is just hex encoding for
//the a four-character text string.
static const HANDLE hDrodWindowProp = (HANDLE)0x54535321;  //TSS!
#endif
#if defined(__linux__) || defined __FreeBSD__ || defined(__APPLE__)
static char *lockfile = NULL;
#endif

#define APP_PROP "DrodProp"
#define W_APP_PROP L"DrodProp"

ULONGLONG qwAvailablePhysical=0, qwAvailableTotal=0;

//
//Module-scope vars.
//

CFiles * m_pFiles = NULL;
unsigned long dwInitAORefCount = 0;

#define GAMETITLE "DROD 5"

#ifndef __linux__  // the linux build thing has build profiles; use the beta profile if you want to enable beta
#ifndef BETA
//#	define BETA
#endif
#endif

#ifdef BETA
#define BETA_EXPIRES 20151201
#	if defined(__FreeBSD__) || defined(__linux__)
char windowTitle[256];
#	else
char windowTitle[] = GAMETITLE " BETA - DO NOT DISTRIBUTE (buggy releases make us look bad)";
#	endif
#else
char windowTitle[] = GAMETITLE;
#endif

#ifdef WIN32
//On Windows, support selecting whether to create and find the user's Data directory
//in their userspace or in a common location (e.g., "ProgramData")
bool bWindowsDataFilesInUserSpecificDir = false;
#endif

//This is a filename that will probably exist in this specific game only.
static const WCHAR wszUniqueResFile[] = {
#ifdef STEAMBUILD_TSS_APP
    We('d'),We('r'),We('o'),We('d'),We('-'),We('t'),We('s'),We('s'),We('5'),We('_'),We('0'),We('.'),We('d'),We('a'),We('t'),We(0)
#else
    We('d'),We('r'),We('o'),We('d'),We('5'),We('_'),We('0'),We('.'),We('d'),We('a'),We('t'),We(0)
#endif
};

//
//Private function prototypes.
//

static MESSAGE_ID   CheckAvailableMemory();
static void         Deinit();
static void         DeinitDB();
static void         DeinitGraphics();
static void         DeinitSound();
static void         DisplayInitErrorMessage(MESSAGE_ID dwMessageID);
static void         GetAppPath(const char *pszArg0, WSTRING &wstrAppPath);
static MESSAGE_ID   Init(bool bNoFullscreen, bool bNoSound);
static void         InitCDate();
static MESSAGE_ID   InitDB();
static MESSAGE_ID   InitGraphics();
static void         InitMetadata();
static MESSAGE_ID   InitSound(bool bNoSound);
static bool         IsAppAlreadyRunning();
static void         RepairMissingINIKeys(const bool bFullVersion);

//*****************************************************************************
int main(int argc, char *argv[])
{
	LOGCONTEXT("main");

	InitMetadata();

#ifdef WIN32
#	ifdef STEAMBUILD
	//Steam default: place player data files in a user-specific location.
	//Note that for Windows Steam users with non-ASCII characters in their Windows username,
	//Metakit won't open such filepaths, so we will have to keep the common location default in this case.
	const WSTRING userpath = CFiles::GetUserspacePath(true);
	if (!userpath.empty() && IsAllPrintableASCIIchars(userpath))
		bWindowsDataFilesInUserSpecificDir = true;
#	endif
#endif

	//command line arguments
	bool bNoFullscreen = false, bNoSound = false, bIsDemo = false;
	for (int nArgNo=0; nArgNo < argc; ++nArgNo) {
		const char* arg = argv[nArgNo];
		if (!_stricmp(arg, "nofullscreen")) {
			bNoFullscreen = true;
		} else if (!_stricmp(arg, "nosound")) {
			bNoSound = true;
		} else if (!_stricmp(arg, "demo")) {
			bIsDemo = true;
			Metadata::Set(MetaKey::DEMO, "1");
		} else if (!strcmp(arg, "embedmedia")) {
#ifdef DEV_BUILD
			Metadata::Set(MetaKey::EMBEDMEDIA, "1");
#else
			return 2;
#endif
#ifdef WIN32
		} else if (!strncmp(arg, "commondatadir=", 14)) {
			const UINT val = convertToUINT(arg + 14);
			bWindowsDataFilesInUserSpecificDir = val == 0 ? true : false;
#endif
		} else if (!strncmp(arg, "datafilenum=", 12)) { //active in Steam build only
			if (!CDbBase::SetCreateDataFileNum(convertToUINT(arg+12)))
				return 2;
		} else if (!strncmp(arg, "applyholdstatus=", 16)) {
			Metadata::Set(MetaKey::APPLYHOLDSTATUS, arg+16);
		}
		//else: more command line arg processing occurs below
	}

# if defined(__linux__) || defined(__FreeBSD__)
	Dyn::LoadX11();
# endif

	//Initialize the app.
	WSTRING wstrPath;
	GetAppPath(argv[0], wstrPath);

#if defined(__linux__) || defined (__FreeBSD__) || defined(__APPLE__)
	//Check if the executable name includes the string "demo", in case
	//DROD was launched without the launch script (otherwise CFiles will
	//look for data in the wrong place).
	if (!bIsDemo)
	{
		WCHAR wszDemo[] = { We('d'),We('e'),We('m'),We('o'),We(0) };
		WSTRING::size_type slashpos = wstrPath.rfind(wszSlash[0], wstrPath.length());
		if (slashpos == WSTRING::npos) slashpos = 0;
		bIsDemo = (wstrPath.substr(slashpos).find(wszDemo, 0) != WSTRING::npos);
	}
#elif defined(WIN32)
	//Support separate dir for demo player files.
	if (!bIsDemo)
	{
		const WCHAR wszDemo[] = { We('D'),We('e'),We('m'),We('o'),We(0) };
		bIsDemo = (wstrPath.find(wszDemo, 0) != WSTRING::npos);
	}
#endif

	std::vector<string> datFiles; //writable .dats.  [0]: + = copy, - = no copy
	std::vector<string> playerDataSubDirs;  // subdirs to create. [0]: + = copy files, - = don't copy files, anything else = don't copy & don't offset name
	playerDataSubDirs.push_back("Bitmaps");
#if defined(__linux__) || defined (__FreeBSD__) //|| defined(__APPLE__)
	playerDataSubDirs.push_back("+Homemade");
#else
	playerDataSubDirs.push_back("Homemade");
#endif
	playerDataSubDirs.push_back("Music");
	playerDataSubDirs.push_back("Sounds");
	CFiles::InitAppVars(wszUniqueResFile, datFiles, playerDataSubDirs);

#ifdef WIN32
	CFiles::bWindowsDataFilesInUserSpecificDir = bWindowsDataFilesInUserSpecificDir;
#endif

	const WCHAR* gameName = wszDROD;
#ifdef STEAMBUILD_TSS_APP
	const WCHAR wszDRODTSS[] = { We('d'),We('r'),We('o'),We('d'),We('-'),We('t'),We('s'),We('s'),We(0) };
	gameName = wszDRODTSS;
#endif
	m_pFiles = new CFiles(wstrPath.c_str(), gameName, wszDROD_VER, bIsDemo);
	if (CFiles::bad_data_path_file) {
		DisplayInitErrorMessage(MID_DataPathDotTextFileIsInvalid);
	}

#if defined(__linux__) || defined (__FreeBSD__) || defined(__APPLE__)
	//Need init'ed CFiles for Linux/Apple part
	if (IsAppAlreadyRunning()) {
		DisplayInitErrorMessage(MID_DRODIsAlreadyRunning);
		delete m_pFiles;
		if (lockfile)
			delete[] lockfile;
#	if defined(__FreeBSD__) || defined(__linux__)
		Dyn::UnloadX11();
#	endif
		return 1;
	}
#endif

#ifdef BETA
	//Disable app outside certain time range.
	string str;
	if (m_pFiles->GetGameProfileString(INISection::Waves, "Old", str))
	{
		DisplayInitErrorMessage(MID_AppConfigError);
		delete m_pFiles;
#	if defined(__FreeBSD__) || defined(__linux__)
		Dyn::UnloadX11();
#	endif
		return -1;
	}

#	if defined(__FreeBSD__) || defined(__linux__)
	snprintf(windowTitle, sizeof(windowTitle), GAMETITLE " time-limited BETA (Compiled: %s) ", __DATE__);
#		if BETA_EXPIRES > 0
	time_t t = time(NULL);
	struct tm expiretm;

	memset(&expiretm, 0, sizeof expiretm);
	expiretm.tm_year = (BETA_EXPIRES / 10000) - 1900;
	expiretm.tm_mon  = (BETA_EXPIRES % 10000 / 100) - 1;
	expiretm.tm_mday = BETA_EXPIRES % 100;

	time_t timetoexpire = mktime(&expiretm) - t;
	struct tm* tmexpiretime = localtime(&timetoexpire);
	time_t monthstoexpire = (tmexpiretime->tm_year -70) * 12 + tmexpiretime->tm_mon;
	time_t daystoexpire = tmexpiretime->tm_mday - 1;

	if (timetoexpire < 0)
	{
		fprintf(stderr, "*** This BETA has expired.\n");
		m_pFiles->WriteGameProfileString(INISection::Waves, "Old", "old.ogg");
		DisplayInitErrorMessage(MID_AppConfigError);
		delete m_pFiles;
		Dyn::UnloadX11();
		return -1;
	}
	else
	{
		snprintf(windowTitle, sizeof(windowTitle), GAMETITLE " time-limited BETA (Compiled: %s - Expires in %d month%s and %d day%s time) ", __DATE__, (int)monthstoexpire, (monthstoexpire!=1) ? "s" : "", (int)daystoexpire, (daystoexpire!=1) ? "s" : "");
		fprintf(stderr, "***\n"
			"*** This is a BETA version.  Do not distribute!\n"
			"***\n"
			"*** Compiled: %s.\n"
			"*** Expires:  in %d month%s and %d day%s time.\n"
			"***\n"
			"*** Please visit %s to find details on more recent versions.\n"
			"***\n\n", __DATE__, (int)monthstoexpire, (monthstoexpire!=1) ? "s" : "",
			(int)daystoexpire, (daystoexpire!=1) ? "s" : "",
			CNetInterface::cNetBaseURL.c_str());
	}
#		else
	fprintf(stderr, "***\n"
		"*** This is a BETA version.  Do not distribute!\n"
		"***\n"
		"*** Compiled: %s.\n"
		"***\n"
		"*** Please visit %s to find details on more recent versions.\n"
		"***\n\n", __DATE__, CNetInterface::cNetBaseURL.c_str());
#		endif
#	else // if not FreeBSD or linux:
	time_t t = time(NULL);
	tm* pLocalTime = localtime(&t);
	if (!(pLocalTime->tm_year == 116 && pLocalTime->tm_mon <= 11)) //<=dec 16
	{
		m_pFiles->WriteGameProfileString(INISection::Waves, "Old", "old.ogg");
		DisplayInitErrorMessage(MID_AppConfigError);
		delete m_pFiles;
		return -1;
	}
#	endif
#endif

	MESSAGE_ID ret = Init(bNoFullscreen, bNoSound);

	if (ret != MID_Success && ret != MID_DatCorrupted_Restored && ret != MID_DRODUpgradingDataFiles)
	{
		if (ret != static_cast<MESSAGE_ID>(-1))
			DisplayInitErrorMessage(ret);
	} else {
#ifndef _DEBUG
		try
		{
#endif
			//Process command line options.
			for (int nArgNo=1; nArgNo < argc; ++nArgNo)
			{
				WSTRING wstrFilename;
				//Queue files for import.
				if (strstr(argv[nArgNo],"."))
				{
					AsciiToUnicode(argv[nArgNo], wstrFilename);
					CDrodScreen::importFiles.push_back(wstrFilename);
				}
				//Export language texts to file specified after option flag.
				else if (!_stricmp(argv[nArgNo], "-exportTexts"))
				{
					if (nArgNo+1 < argc)
					{
						AsciiToUnicode(argv[++nArgNo], wstrFilename);
						g_pTheDB->ExportTexts(wstrFilename.c_str());
					}
				}
				//Import language texts from file specified after option flag.
				else if (!_stricmp(argv[nArgNo], "-importTexts"))
				{
					if (nArgNo+1 < argc)
					{
						AsciiToUnicode(argv[++nArgNo], wstrFilename);
#ifdef WIN32
						g_pTheDB->ImportTexts(wstrFilename.c_str());
#elif defined(__linux__) || defined(__FreeBSD__)
						//Linux and friends can't write changes to drod3_0.dat,
						//so we copy imported files to our writable dat path
						//and autoimport anything found there later (below)
						if (!(m_pFiles->CopyLocalizationFile(wstrFilename.c_str())))
						{
							//If the copy failed, we can at least use it for this run
							g_pTheDB->ImportTexts(wstrFilename.c_str());
						}
#endif
					}
				}
			}
#if defined(__linux__) || defined(__FreeBSD__)
			//Autoimport localization files found in the localization dir
			{
				vector<WSTRING> files;
				if (CFiles::GetLocalizationFiles(files))
					for (vector<WSTRING>::const_iterator i = files.begin(); i != files.end(); ++i)
						g_pTheDB->ImportTexts(i->c_str());
			}
#endif

			SCREENTYPE eNextScreen = SCR_None;
			if (ret == MID_DatCorrupted_Restored)
				DisplayInitErrorMessage(ret);

			//Go to player creation or selection screen (if needed), then to title screen.
			//If a player already exists, the NewPlayer screen will be skipped (not activate).
			//If a single player exists, the SelectPlayer screen will be skipped also.
			g_pTheSM->InsertReturnScreen(SCR_Title);
			g_pTheSM->InsertReturnScreen(SCR_SelectPlayer);
			eNextScreen = SCR_NewPlayer;

#ifndef DEV_BUILD
			//Get active player ID.
			string strPlayerID;
			if (m_pFiles->GetGameProfileString(INISection::Startup, "PlayerID", strPlayerID))
			{
				//If ID is for a valid local player, set active player.
				const UINT dwPlayerID = convertToUINT(strPlayerID.c_str());
				if (g_pTheDB->Players.Exists(dwPlayerID) && g_pTheDB->Players.IsLocal(dwPlayerID))
					g_pTheDB->SetPlayerID(dwPlayerID);
			}
#endif

			//Set player preferences.
			const CDbPackedVars s = g_pTheDB->GetCurrentPlayerSettings();
			g_pTheDBM->bAlpha = s.GetVar(Settings::Alpha, true);
			const BYTE gammaOne = CDrodBitmapManager::GetGammaOne();
			const BYTE gamma = s.GetVar(Settings::Gamma, gammaOne);
			if (gamma != gammaOne) //causes tinted display issues on Mac
				g_pTheDBM->SetGamma(gamma);
			g_pTheDBM->eyeCandy = s.GetVar(Settings::EyeCandy, BYTE(Metadata::GetInt(MetaKey::MAX_EYE_CANDY)));
			g_pTheDBM->tarstuffAlpha = s.GetVar(Settings::TarstuffAlpha, BYTE(255));

			//Set sound preferences.
			g_pTheSound->EnableSoundEffects(s.GetVar(Settings::SoundEffects, true));
			g_pTheSound->SetSoundEffectsVolume(s.GetVar(Settings::SoundEffectsVolume, (BYTE)DEFAULT_SOUND_VOLUME));
			g_pTheSound->EnableVoices(s.GetVar(Settings::Voices, true));
			g_pTheSound->SetVoicesVolume(s.GetVar(Settings::VoicesVolume, (BYTE)DEFAULT_VOICE_VOLUME));
			g_pTheSound->EnableMusic(s.GetVar(Settings::Music, true));
			g_pTheSound->SetMusicVolume(s.GetVar(Settings::MusicVolume, (BYTE)DEFAULT_MUSIC_VOLUME));

			//Decide whether app is fullscreen (default) or not.
			const bool bFullscreen = !CScreen::bAllowWindowed || (!bNoFullscreen && s.GetVar(Settings::Fullscreen, false));
			if (bFullscreen)
			{
				SDL_SetWindowFullscreen(GetMainWindow(), SDL_WINDOW_FULLSCREEN_DESKTOP);
			}
			else
			{
#ifndef __linux__  //This doesn't work well on X11. The window manager handles it anyway.
				int nX, nY, nW, nH;
				CScreen::GetScreenSize(nW, nH);
				CScreen::GetWindowPos(nX, nY);

				//Set window position to user preference.
				// XXX add display index?
				nX = s.GetVar("ScreenX", nX);
				nY = s.GetVar("ScreenY", nY);

				//If app window extends off screen, relocate to top-left corner for better viewing.
				if (nW < nX + CScreen::CX_SCREEN || nH < nY + CScreen::CY_SCREEN)
					nX = nY = -3;
				//If app has somehow gone too far left/up, put it back onscreen.
				if (nX < -50) nX = 0;
				if (nY < -50) nY = 0;

				CScreen::SetWindowPos(nX, nY);
#endif
			}

			//Event-handling will happen in the execution of ActivateScreen().
			//ActivateScreen() will return when player exits a screen.
			while (eNextScreen != SCR_None) //If SCR_None, then app is exiting.
			{
#ifdef _DEBUG
				string strActivatingScreen;
				g_pTheSM->GetScreenName(eNextScreen, strActivatingScreen);
				string strContext = "Active screen: ";
				strContext += strActivatingScreen;
				LOGCONTEXT(strContext.c_str());
#endif

				eNextScreen = (SCREENTYPE)g_pTheSM->ActivateScreen(eNextScreen);
			}

			CScreen::SetCursor(CUR_Wait);	//show wait icon while deiniting
			CDrodScreen::logoutFromChat();

			//Record player preferences for next session.
			CDbPlayer *pCurrentPlayer = g_pTheDB->GetCurrentPlayer();
			if (pCurrentPlayer)
			{
				//Record windowed app coordinates on screen.
				const bool bFullscreen = !bNoFullscreen &&
						(pCurrentPlayer->Settings.GetVar(Settings::Fullscreen, false));
				if (!bFullscreen)
				{
					int nX, nY;
					CScreen::GetWindowPos(nX, nY);
					pCurrentPlayer->Settings.SetVar("ScreenX", nX);
					pCurrentPlayer->Settings.SetVar("ScreenY", nY);
					pCurrentPlayer->Update();
				}

				//Save active player ID.
				char cPlayerID[20];
				_itoa(pCurrentPlayer->dwPlayerID, cPlayerID, 10);
				m_pFiles->WriteGameProfileString(INISection::Startup, "PlayerID", cPlayerID);

				delete pCurrentPlayer;
			}

#ifndef _DEBUG
		}
		catch (CException& e)
		{
		  m_pFiles->AppendErrorLog(e.what());
		  if (g_pTheDB->IsOpen())
		  {
			  g_pTheDB->Close();
		  }
		}
		catch (...)
		{
		  if (g_pTheDB->IsOpen())
		  {
			  g_pTheDB->Close();
		  }
		}
#endif
	}

	//Deinitialize the app.
	Deinit();
	return ret==MID_Success ? 0 : 1;
}

//*****************************************************************************
MESSAGE_ID Init(
//Top-level function for all application initialization.
//
//Init() may leave app in a partially initialized state, which caller should
//clean up with Deinit().  Init() does not call Deinit() here because a partial
//initialization can be useful for reporting errors to the user after init
//returns.
//
//Params:
  bool bNoFullscreen,         //(in)  If true, then app will run windowed regardless
										//      of player settings.
  bool bNoSound)              //(in)  If true, then all sound will be disabled
										//      regardless of player settings.
//
//Returns:
//MID_Success or Message ID of a failure message to display to user.
{
	LOGCONTEXT("Init");

	MESSAGE_ID ret;
	bool bRestoredFromCorruption = false;
	bool bUpgradingDataFiles = false;

	dwInitAORefCount = GetAORefCount();
	Language::SetLanguage(CDrodBitmapManager::GetLanguage());
	CScreen::bAllowFullScreen = !bNoFullscreen;
#ifdef RUSSIAN_BUILD
	CDate::SetDateFormat(CDate::DMY);
#endif

	//Verify that type sizes are correct for the current arch
	ASSERT(sizeof(BYTE) == 1);
	ASSERT(sizeof(UINT) == 4);
	ASSERT(sizeof(ULONGLONG) == 8);

	static const char* pUserAgent = "DROD 5.0";
	VERIFY(CInternet::Init(pUserAgent));

	//Check memory availability.  App may need to exit or a warning message may be needed.
	ret = CheckAvailableMemory();
	if (ret == MID_MemLowExitNeeded)
	  return ret;
	if (ret == MID_MemLowWarning || ret == MID_MemPerformanceWarning)
	  DisplayInitErrorMessage(ret); //Init() can continue after the warning.

#ifdef STEAMBUILD
	bool bSteamInit = false;
	const UINT MAX_STEAMINIT_TRIES=3;
	for (UINT steamInitTries=1; steamInitTries<=MAX_STEAMINIT_TRIES && !bSteamInit; ++steamInitTries) {
		if (!(bSteamInit = SteamAPI_Init())) {
			if (steamInitTries < MAX_STEAMINIT_TRIES)
				SDL_Delay(steamInitTries * 1000); //s
		}
	}

	// never go into windowed mode in steam big picture
	CScreen::bAllowWindowed = !(SteamUtils() && SteamUtils()->IsSteamInBigPictureMode());
#endif

	//Initialize graphics before other things, because I want to show the
	//screen quickly for the user.
	ret = InitGraphics();
	if (ret)
	{
		//If graphics initialization fails, try to load the DB before exit in order
		//to provide a localized error message.
		if (ret != static_cast<MESSAGE_ID>(-1))
			InitDB();
		return ret;
	}

	//Open the database before other things, because it gives access to
	//localized messages that can be used to describe any Init() failures.
	CScreen::InitMIDs(MID_ReallyQuit, MID_OverwriteFilePrompt);
	ret = InitDB();
	switch (ret)
	{
		case MID_DatCorrupted_Restored: bRestoredFromCorruption = true; break;
		case MID_DRODUpgradingDataFiles: bUpgradingDataFiles = true; break;
		default:
			if (ret)
				return ret;
		break;
	}

	//Set up INI entries once the DB can be accessed.
	RepairMissingINIKeys(CDrodScreen::IsGameFullVersion());

	//Init the internet interface.
	ASSERT(!g_pTheNet);
#ifdef CARAVELBUILD
	g_pTheNet = (CNetInterface*)new CCaravelNetInterface();
#else
	g_pTheNet = (CNetInterface*)new CNetInterface();
#endif
	if (!g_pTheNet) return MID_OutOfMemory;

	//Load this screen now because it is necessary to play the game, and
	//I want to know about load failures early.
	if (!g_pTheSM->GetScreen(SCR_Game))
		return MID_CouldNotLoadResources;

	//Initialize sound.  Music will not play until first screen loads.
	ret = InitSound(bNoSound);
	if (ret)
		return ret;

#ifdef STEAMBUILD
	if (!bSteamInit)
		DisplayInitErrorMessage(MID_SteamAPIInitError);
	if (SteamUserStats())
		SteamUserStats()->RequestCurrentStats();
#endif

	srand(int(time(NULL)));

	//Success.
	return bRestoredFromCorruption ? MID_DatCorrupted_Restored :
			bUpgradingDataFiles ? MID_DRODUpgradingDataFiles : MID_Success;
}

//*****************************************************************************
void Deinit()
//Top-level function for all application deinitialization.
//
//Deinit() should be able to handle partial initialization without problems.
//All error reporting should be done through CFiles::AppendErrorLog().
{
	LOGCONTEXT("Deinit");

	if (g_pTheDSM)
	{
		//Unload game at this point when deiniting.
		if (g_pTheDSM->IsScreenInstanced(SCR_Game))
		{
			CGameScreen *pGameScreen = DYN_CAST(CGameScreen*, CScreen*,
					g_pTheDSM->GetScreen(SCR_Game));
			if (pGameScreen && pGameScreen->IsGameLoaded())
				pGameScreen->UnloadGame();
		}
	}

	//Generally, deinit subsystems in reverse order of their initialization.
#ifdef STEAMBUILD
	SteamAPI_Shutdown();
#endif

	// TODO FIXME : Wait for pending Cloud stuff to close.
	delete g_pTheNet;
	g_pTheNet = NULL;
	DeinitGraphics(); //free graphics objects first to avoid DB assertion
	DeinitSound(); //closing SDL audio may hang a few seconds on Linux for some reason, so do it after graphics
	DeinitDB();
	CInternet::Deinit();

#if defined(__linux__) || defined(__FreeBSD__)
	//Unload optional dynamic libraries
	Dyn::UnloadAll();
#endif

	//GetAORefCount() returns the number of CAttachableObjects still in
	//memory.  Must match the number of static instances at start time.
	ASSERT(GetAORefCount()==dwInitAORefCount);

	delete m_pFiles;

	SDL_Quit(); //close game window last of all
}

//*****************************************************************************
MESSAGE_ID InitDB()
//Initialize the database so that it is available for the entire app.
//
//Returns:
//MID_Success or an error message ID.
{
	LOGCONTEXT("InitDB");
	ASSERT(!g_pTheDB);

	//Create hostage reference to database that will be around for life of app.
	g_pTheDB = new CDb;
	if (!g_pTheDB)
		return MID_OutOfMemory;

	MESSAGE_ID ret = g_pTheDB->Open();
	if (ret == MID_DatMissing)
	{
		//Try to find correct location of drod<ver>.dat and fix incorrect DataPath.txt file
		CFiles Files;
		Files.TryToFindDataPath();
		ret = g_pTheDB->Open(); //Uses data path set in above call to find location of DB files.
	}

	//Initialize the CDate class with month text from database.
	if (g_pTheDB->IsOpen() && ret == MID_Success)
		InitCDate();

	ASSERT(ret != MID_Success || g_pTheDB->IsOpen());
	return ret;
}

//*****************************************************************************
void DeinitDB()
//Deinits the database.
{
	LOGCONTEXT("DeinitDB");
	//Release hostage reference, causing database to close.
	if (g_pTheDB)
	{
		g_pTheDB->Close();
		delete g_pTheDB;
		g_pTheDB = NULL;
	}

	//GetDbRefCount() returns the number of CDbBase-derived objects still in
	//memory.  If not zero, then there are some objects that should have been
	//deleted somewhere.
	ASSERT(GetDbRefCount()==0L);
}

//*****************************************************************************
MESSAGE_ID InitGraphics()
//Initializes SDL, screen manager, bitmap manager, font manager, and brings up
//a window.
//
//Returns:
//MID_Success or an error message ID.
{
	LOGCONTEXT("InitGraphics");
/*
#ifdef WIN32
	XXX why was this needed? we could use SDL2's software renderer, but it's best to avoid it if we can
	//Use Windib input driver on Windows unless explicitly set otherwise.
#	if SDL_VERSION_ATLEAST(1,2,10)
		string strDriver;
		if (CFiles::GetGameProfileString(INISection::Customizing, "Windib", strDriver) && atoi(strDriver.c_str()) == 0)
			SDL_putenv("SDL_VIDEODRIVER=directx");
#	endif
#endif
*/
	//Ensure screensaver remains enabled
	SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

	//Default to filtered scaling (not ideal, but better than uneven pixel sizes)
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

	//Minimize on focus loss doesn't work well with multiple monitors
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

	//Vsync eliminates tearing and uses less resources (we don't need to render at thousands of fps)
	SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

	//Initialize the library.
	if (SDL_Init(SDL_INIT_VIDEO) < 0) //Note: Audio is initialized in CSound, if required
	{
sdl_error:
		CFiles Files;
		char szErrMsg[100];
		szErrMsg[sizeof(szErrMsg)-1] = 0; //windows' snprintf doesn't zero-terminate on overflow
		_snprintf(szErrMsg, sizeof(szErrMsg)-1, "SDL error: %s\n", SDL_GetError());
		Files.AppendErrorLog(szErrMsg);
		return MID_SDLInitFailed;
	}

	//Don't open on a too small monitor if a larger one is available
	int default_display = CScreen::GetDisplayForDesktopResOfAtLeast(CScreen::CX_SCREEN, CScreen::CY_SCREEN);
	if (default_display < 0)
		default_display = 0;

	//Get a 1024x768x<bpp> screen.

	UniquePtr<SDL_Window, SDL_DestroyWindow> window((
		SDL_CreateWindow(windowTitle,
			SDL_WINDOWPOS_CENTERED_DISPLAY(default_display), SDL_WINDOWPOS_CENTERED_DISPLAY(default_display),
			CScreen::CX_SCREEN, CScreen::CY_SCREEN, CScreen::bAllowWindowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP)
	));
	if (!window.get())
		goto sdl_error;

	UniquePtr<SDL_Renderer, SDL_DestroyRenderer> renderer((
		SDL_CreateRenderer(window.get(), -1, 0)
	));
	if (!renderer.get())
		goto sdl_error;

	UniquePtr<SDL_Texture, SDL_DestroyTexture> screentexture((
		SDL_CreateTexture(renderer.get(),
			SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, CScreen::CX_SCREEN, CScreen::CY_SCREEN)
	));
	if (!screentexture.get())
		goto sdl_error;

	Uint32 tex_fmt, tex_rmask, tex_gmask, tex_bmask, tex_amask;
	int tex_bpp;
	SDL_QueryTexture(screentexture.get(), &tex_fmt, NULL, NULL, NULL);
	SDL_PixelFormatEnumToMasks(tex_fmt, &tex_bpp, &tex_rmask, &tex_gmask, &tex_bmask, &tex_amask);

	g_pTheBM->BITS_PER_PIXEL = tex_bpp;

	UniquePtr<SDL_Surface, SDL_FreeSurface> shadowsurface((
		SDL_CreateRGBSurface(0,
			CScreen::CX_SCREEN, CScreen::CY_SCREEN, tex_bpp, tex_rmask, tex_gmask, tex_bmask, tex_amask)
	));
	if (!shadowsurface.get())
		goto sdl_error;

	SetMainWindow(window.get(), shadowsurface.get(), screentexture.get());

	//Center window on screen immediately.
	CScreen::SetWindowCentered();
	SDL_RenderClear(renderer.get());
	SDL_RenderPresent(renderer.get());

	//Set icon.
	{
		static const WCHAR wszBmps[] = { We('B'),We('i'),We('t'),We('m'),We('a'),We('p'),We('s'),We(0) };
		static const WCHAR wszIcon[] = {
			We('I'),We('c'),We('o'),We('n'),We('-'),We('3'),We('2'),We('x'),We('3'),We('2'),
			We('.'),We('b'),We('m'),We('p'),We(0)
		};
		WSTRING wstrIconFilepath;
		CFiles Files;
		wstrIconFilepath = Files.GetResPath();
		wstrIconFilepath += wszSlash;
		wstrIconFilepath += wszBmps;
		wstrIconFilepath += wszSlash;
		wstrIconFilepath += wszIcon;
		CStretchyBuffer bitmap;
		CFiles::ReadFileIntoBuffer(wstrIconFilepath.c_str(), bitmap, true);
		SDL_Surface *iconsurf = SDL_LoadBMP_RW(SDL_RWFromMem((BYTE*)bitmap, bitmap.Size()), 1);
		if (iconsurf) {
			SDL_SetWindowIcon(window.get(), iconsurf);
			SDL_FreeSurface(iconsurf);
		}
#if defined(__APPLE__) || defined(__linux__) || defined __FreeBSD__
		// set 128x128 icon, if a bitmap exists
		static const WCHAR wszIcon128x128[] = {
			We('I'),We('c'),We('o'),We('n'),We('-'),We('1'),We('2'),We('8'),We('x'),We('1'),We('2'),We('8'),
			We('.'),We('b'),We('m'),We('p'),We(0)
		};
		wstrIconFilepath = WSTRING(Files.GetResPath()) + wszSlash + wszBmps + wszSlash + wszIcon128x128;
		bitmap = CStretchyBuffer();
		if (CFiles::ReadFileIntoBuffer(wstrIconFilepath.c_str(), bitmap, true)) {
			iconsurf = SDL_LoadBMP_RW(SDL_RWFromMem((BYTE*)bitmap, bitmap.Size()), 1);
			SDL_SetWindowIcon(window.get(), iconsurf);
			SDL_FreeSurface(iconsurf);
		}
#endif
	}

	//In Windows, set focus to window in case it was lost during startup.
	//This is to avoid losing the input focus.
#  ifdef WIN32
	{
		SDL_SysWMinfo Info;
		SDL_VERSION(&Info.version);
		SDL_GetWindowWMInfo(window.get(), &Info);
		HWND hwndRet = SetFocus(Info.info.win.window);
		if (CFiles::WindowsCanBrowseUnicode())
			SetPropW(Info.info.win.window, W_APP_PROP, hDrodWindowProp);
		else
			SetPropA(Info.info.win.window, APP_PROP, hDrodWindowProp);

		//Disallow running more than one instance of the app at a time.
		if (IsAppAlreadyRunning())
			return static_cast<MESSAGE_ID>(-1);
	}
#  elif defined(__linux__) || defined(__FreeBSD__)
	//Write the X11 window id to the pid file so it can be read by later
	//instances, which can then do a futile attempt at raising the window.
	{
		FILE *fp;
		SDL_SysWMinfo Info;
		SDL_VERSION(&Info.version);
		if (SDL_GetWindowWMInfo(window.get(), &Info) && Info.subsystem == SDL_SYSWM_X11 && (fp = fopen(lockfile, "ab")))
		{
			fprintf(fp, ":%x:", (UINT)Info.info.x11.window);
			fclose(fp);
		}
	}
#  endif

	//Init the bitmap manager.
	ASSERT(!g_pTheBM);
	g_pTheDBM = new CDrodBitmapManager();
	g_pTheBM = (CBitmapManager*)g_pTheDBM;
	if (!g_pTheBM) return MID_OutOfMemory;
	MESSAGE_ID ret = (MESSAGE_ID)g_pTheDBM->Init();
	if (ret) return ret;

	//Init the font manager.
	ASSERT(!g_pTheFM);
	g_pTheDFM = new CDrodFontManager();
	g_pTheFM = (CFontManager*)g_pTheDFM;
	if (!g_pTheFM) return MID_OutOfMemory;
	ret = (MESSAGE_ID)g_pTheDFM->Init();
	if (ret) return ret;

	//Init the screen manager.
	ASSERT(!g_pTheSM);
	g_pTheDSM = new CDrodScreenManager(shadowsurface.get());
	g_pTheSM = (CScreenManager*)g_pTheDSM;
	if (!g_pTheSM) return MID_OutOfMemory;
	ret = (MESSAGE_ID)g_pTheDSM->Init();
	if (ret) return ret;

	//Set screen transition duration.
	g_pTheDSM->InitCrossfadeDuration();

	//Show hourglass.
	SDL_SetCursor(g_pTheSM->GetCursor(CUR_Wait));

	//Show splash screen graphic.
	const int X_TITLE = (CScreen::CX_SCREEN - 182) / 2; //center
	const int Y_TITLE = 200;
	static const WCHAR wszSplashscreenGraphic[] = {We('D'),We('o'),We('o'),We('r'),We(0)};
	CImageWidget *pTitleImage = new CImageWidget(0, X_TITLE, Y_TITLE, wszSplashscreenGraphic);
	pTitleImage->Paint();
	PresentRect();
	delete pTitleImage;

	//Ignore any keys being held down on startup.
	SDL_SetModState(KMOD_NONE);

	//Success.
	window.release();
	renderer.release();
	screentexture.release();
	shadowsurface.release();
	return MID_Success;
}

//*****************************************************************************
void DeinitGraphics()
//Deinit graphics.
{
	LOGCONTEXT("DeinitGraphics");

	//Delete the screen manager.
	delete g_pTheSM;
	g_pTheSM = NULL;

	//Delete the font manager.
	delete g_pTheFM;
	g_pTheFM = NULL;

	//Delete the bitmap manager.
	delete g_pTheBM;
	g_pTheBM = NULL;

	//Close the window.
	if (SDL_Window *window = GetMainWindow())
	{
		if (CScreen::bAllowWindowed)
			SDL_SetWindowFullscreen(window, 0);

		if (SDL_Texture *texture = GetWindowTexture(window))
			SDL_DestroyTexture(texture);
		if (SDL_Surface *shadow = GetWindowShadowSurface(window))
			SDL_FreeSurface(shadow);
		if (SDL_Renderer *renderer = SDL_GetRenderer(window))
			SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

//*****************************************************************************
MESSAGE_ID InitSound(
//After this call, g_pTheSound can be used.
//
//Params:
  bool bNoSound)  //(in)  If true, then all sound will be disabled.  Unlike,
						//      partial disablement, this will prevent any sound library
						//      calls to be made during this session.
//
//Returns:
//MID_Success or an error message ID.
{
	LOGCONTEXT("InitSound");
	ASSERT(!g_pTheSound);

	//Create global instance of CSound object.
	g_pTheSound = (CSound*)new CDrodSound(bNoSound);
	ASSERT(g_pTheSound);
	if (bNoSound) return MID_Success;

	//CSound() disables itself if a failure occurs during construction.
	//Future calls to a disabled g_pTheSound don't do anything, which is okay.
	//Sound is not a big enough deal to fail app initialization for.  The
	//player can not have sound, and still be able to play the game.

	return MID_Success;
}

//*****************************************************************************
void DeinitSound()
//Deinits sound.
{
	LOGCONTEXT("DeinitSound");
	if (g_pTheSound)
	{
		g_pTheSound->WaitForSoundEffectsToStop();
		delete g_pTheSound;
		g_pTheSound = NULL;
	}
}

//*****************************************************************************
void InitMetadata()
{
	Metadata::Set(MetaKey::APPLYHOLDSTATUS, "-1"); //CDbHold::NoStatus
	Metadata::Set(MetaKey::DEMO, "0");
	Metadata::Set(MetaKey::EMBEDMEDIA, "0");
	Metadata::Set(MetaKey::MAX_EYE_CANDY, "16");
}

//*****************************************************************************
#ifdef USE_GTK
static gboolean gtkcb_delete_event (GtkWidget *widget, GdkEvent *event, gpointer data)
{
	return 0;
}

static void gtkcb_destroy (GtkWidget *widget, gpointer data)
{
	Dyn::gtk_main_quit();
}
#endif

void DisplayInitErrorMessage(
//Displays an error message for the user, taking into account the possible
//states of partial initialization the application may be in.
//
//Params:
	MESSAGE_ID dwMessageID) //(in)   Message to display.
{
	//Get message text from database if possible.
	const WCHAR *pwczMessage = NULL;
	if (dwMessageID > MID_LastUnstored && CDbBase::IsOpen())
		pwczMessage = g_pTheDB->GetMessageText(dwMessageID);

	//Can't get message from database, so there is a database failure.  The
	//error should involve the database.  Provide hard-coded text to
	//describe this.
	WSTRING wstrMessage;
	if (!pwczMessage)
	{
#ifdef RUSSIAN_BUILD
		//These are the hardcoded strings below, translated into Russian.
		static const WCHAR wstrDatMissing[] = {1053, 1077, 1074, 1086, 1079, 1084, 1086, 1078, 1085, 1086, 32, 1085, 1072, 1081, 1090, 1080, 32, 1076, 1072, 1085, 1085, 1099, 1077, 32, 1080, 1075, 1088, 1099, 46, 32, 32, 1069, 1090, 1091, 32, 1087, 1088, 1086, 1073, 1083, 1077, 1084, 1091, 32, 1084, 1086, 1078, 1077, 1090, 32, 1088, 1077, 1096, 1080, 1090, 1100, 32, 1087, 1077, 1088, 1077, 1091, 1089, 1090, 1072, 1085, 1086, 1074, 1082, 1072, 32, 34, 68, 82, 79, 68, 34, 46, 0};
		static const WCHAR wstrDatNoAccess[] = {1053, 1077, 1090, 32, 1076, 1086, 1089, 1090, 1091, 1087, 1072, 32, 1082, 32, 1076, 1072, 1085, 1085, 1099, 1084, 32, 1080, 1075, 1088, 1099, 46, 32, 32, 1045, 1089, 1083, 1080, 32, 1074, 1099, 32, 1079, 1072, 1087, 1091, 1089, 1082, 1072, 1077, 1090, 1077, 32, 1080, 1075, 1088, 1091, 32, 34, 68, 82, 79, 68, 34, 32, 1080, 1079, 32, 1089, 1077, 1090, 1077, 1074, 1086, 1075, 1086, 32, 1086, 1082, 1088, 1091, 1078, 1077, 1085, 1080, 1103, 44, 32, 1101, 1090, 1086, 32, 1084, 1086, 1078, 1077, 1090, 32, 1073, 1099, 1090, 1100, 32, 1087, 1088, 1080, 1095, 1080, 1085, 1086, 1081, 32, 1087, 1088, 1086, 1073, 1083, 1077, 1084, 1099, 46, 32, 32, 1058, 1072, 1082, 1078, 1077, 32, 1074, 1086, 1079, 1084, 1086, 1078, 1085, 1086, 44, 32, 1095, 1090, 1086, 32, 1076, 1088, 1091, 1075, 1086, 1077, 32, 1087, 1088, 1080, 1083, 1086, 1078, 1077, 1085, 1080, 1077, 32, 1087, 1099, 1090, 1072, 1077, 1090, 1089, 1103, 32, 1080, 1089, 1087, 1086, 1083, 1100, 1079, 1086, 1074, 1072, 1090, 1100, 32, 1076, 1072, 1085, 1085, 1099, 1077, 32, 34, 68, 82, 79, 68, 34, 32, 1074, 32, 1101, 1090, 1086, 32, 1078, 1077, 32, 1074, 1088, 1077, 1084, 1103, 59, 32, 1084, 1086, 1078, 1077, 1090, 1077, 32, 1087, 1086, 1087, 1088, 1086, 1073, 1086, 1074, 1072, 1090, 1100, 32, 1087, 1077, 1088, 1077, 1079, 1072, 1087, 1091, 1089, 1090, 1080, 1090, 1100, 32, 34, 68, 82, 79, 68, 34, 44, 32, 1079, 1072, 1082, 1088, 1099, 1074, 32, 1076, 1088, 1091, 1075, 1080, 1077, 32, 1087, 1088, 1080, 1083, 1086, 1078, 1077, 1085, 1080, 1103, 46, 0};
		static const WCHAR wstrDatCorrupted_NoBackup[] = {1044, 1072, 1085, 1085, 1099, 1077, 32, 1080, 1075, 1088, 1099, 32, 1073, 1099, 1083, 1080, 32, 1087, 1086, 1074, 1088, 1077, 1078, 1076, 1077, 1085, 1099, 32, 1080, 1079, 45, 1079, 1072, 32, 1086, 1096, 1080, 1073, 1082, 1080, 46, 32, 1048, 1075, 1088, 1072, 32, 1087, 1099, 1090, 1072, 1083, 1072, 1089, 1100, 32, 1074, 1086, 1089, 1089, 1090, 1072, 1085, 1086, 1074, 1080, 1090, 1100, 32, 1076, 1072, 1085, 1085, 1099, 1077, 32, 1089, 32, 1087, 1086, 1089, 1083, 1077, 1076, 1085, 1077, 1081, 32, 1093, 1086, 1088, 1086, 1096, 1077, 1081, 32, 1082, 1086, 1087, 1080, 1080, 44, 32, 1085, 1086, 32, 1086, 1087, 1077, 1088, 1072, 1094, 1080, 1103, 32, 1085, 1077, 32, 1091, 1076, 1072, 1083, 1072, 1089, 1100, 46, 32, 1056, 1077, 1082, 1086, 1084, 1077, 1085, 1076, 1091, 1077, 1090, 1089, 1103, 32, 1087, 1077, 1088, 1077, 1091, 1089, 1090, 1072, 1085, 1086, 1074, 1080, 1090, 1100, 32, 34, 68, 82, 79, 68, 34, 44, 32, 1085, 1086, 44, 32, 1082, 32, 1089, 1086, 1078, 1072, 1083, 1077, 1085, 1080, 1102, 44, 32, 1074, 1089, 1103, 32, 1074, 1072, 1096, 1072, 32, 1080, 1085, 1092, 1086, 1088, 1084, 1072, 1094, 1080, 1103, 32, 1073, 1091, 1076, 1077, 1090, 32, 1087, 1086, 1090, 1077, 1088, 1103, 1085, 1072, 46, 0};
		static const WCHAR wstrDatCorrupted_Restored[] = {1044, 1072, 1085, 1085, 1099, 1077, 32, 1080, 1075, 1088, 1099, 32, 1073, 1099, 1083, 1080, 32, 1087, 1086, 1074, 1088, 1077, 1078, 1076, 1077, 1085, 1099, 32, 1080, 1079, 45, 1079, 1072, 32, 1086, 1096, 1080, 1073, 1082, 1080, 46, 32, 1048, 1075, 1088, 1072, 32, 1074, 1086, 1089, 1089, 1090, 1072, 1085, 1086, 1074, 1083, 1077, 1085, 1072, 32, 1089, 32, 1087, 1086, 1089, 1083, 1077, 1076, 1085, 1077, 1081, 32, 1093, 1086, 1088, 1086, 1096, 1077, 1081, 32, 1082, 1086, 1087, 1080, 1080, 46, 32, 1050, 32, 1089, 1086, 1078, 1072, 1083, 1077, 1085, 1080, 1102, 44, 32, 1074, 1099, 32, 1087, 1086, 1090, 1077, 1088, 1103, 1083, 1080, 32, 1089, 1086, 1093, 1088, 1072, 1085, 1077, 1085, 1085, 1099, 1077, 32, 1080, 1075, 1088, 1099, 32, 1080, 32, 1080, 1079, 1084, 1077, 1085, 1077, 1085, 1080, 1103, 32, 1089, 32, 1087, 1086, 1089, 1083, 1077, 1076, 1085, 1077, 1075, 1086, 32, 1088, 1072, 1079, 1072, 46, 0};
		static const WCHAR wstrCouldNotOpenDB[] = {1053, 1077, 1074, 1086, 1079, 1084, 1086, 1078, 1085, 1086, 32, 1086, 1090, 1082, 1088, 1099, 1090, 1100, 32, 1076, 1072, 1085, 1085, 1099, 1077, 32, 1080, 1075, 1088, 1099, 46, 32, 1069, 1090, 1086, 32, 1079, 1085, 1072, 1095, 1080, 1090, 44, 32, 1095, 1090, 1086, 32, 1085, 1091, 1078, 1085, 1099, 1081, 32, 1092, 1072, 1081, 1083, 32, 34, 68, 82, 79, 68, 34, 32, 1087, 1086, 1074, 1088, 1077, 1078, 1076, 1077, 1085, 46, 32, 1069, 1090, 1091, 32, 1087, 1088, 1086, 1073, 1083, 1077, 1084, 1091, 32, 1084, 1086, 1078, 1085, 1086, 32, 1088, 1077, 1096, 1080, 1090, 1100, 32, 1087, 1077, 1088, 1077, 1091, 1089, 1090, 1072, 1085, 1086, 1074, 1082, 1086, 1081, 32, 34, 68, 82, 79, 68, 34, 46, 0};
		static const WCHAR wstrMemPerformanceWarning[] = {34, 68, 82, 79, 68, 34, 32, 1076, 1086, 1083, 1078, 1077, 1085, 32, 1079, 1072, 1087, 1091, 1089, 1082, 1072, 1090, 1100, 1089, 1103, 32, 1073, 1077, 1079, 32, 1087, 1088, 1086, 1073, 1083, 1077, 1084, 44, 32, 1085, 1086, 32, 1077, 1075, 1086, 32, 1087, 1088, 1086, 1080, 1079, 1074, 1086, 1076, 1080, 1090, 1077, 1083, 1100, 1085, 1086, 1089, 1090, 1100, 32, 1084, 1086, 1078, 1085, 1086, 32, 1091, 1083, 1091, 1095, 1096, 1080, 1090, 1100, 44, 32, 1086, 1089, 1074, 1086, 1073, 1086, 1076, 1080, 1074, 32, 1076, 1086, 1087, 1086, 1083, 1085, 1080, 1090, 1077, 1083, 1100, 1085, 1091, 1102, 32, 1087, 1072, 1084, 1103, 1090, 1100, 32, 1085, 1072, 32, 1082, 1086, 1084, 1087, 1100, 1102, 1090, 1077, 1088, 1077, 46, 32, 32, 1045, 1089, 1083, 1080, 32, 1074, 1099, 32, 1079, 1072, 1082, 1088, 1086, 1077, 1090, 1077, 32, 1082, 1072, 1082, 1080, 1077, 45, 1083, 1080, 1073, 1086, 32, 1080, 1079, 32, 1086, 1090, 1082, 1088, 1099, 1090, 1099, 1093, 32, 1087, 1088, 1080, 1083, 1086, 1078, 1077, 1085, 1080, 1081, 44, 32, 1101, 1090, 1086, 32, 1084, 1086, 1078, 1077, 1090, 32, 1087, 1086, 1084, 1086, 1095, 1100, 46, 0};
		static const WCHAR wstrMemLowWarning[] = {1042, 32, 1074, 1072, 1096, 1077, 1081, 32, 1089, 1080, 1089, 1090, 1077, 1084, 1077, 32, 1085, 1077, 1076, 1086, 1089, 1090, 1072, 1090, 1086, 1095, 1085, 1086, 32, 1087, 1072, 1084, 1103, 1090, 1080, 46, 32, 32, 1042, 1077, 1088, 1086, 1103, 1090, 1085, 1086, 44, 32, 34, 68, 82, 79, 68, 34, 32, 1079, 1072, 1087, 1091, 1089, 1090, 1080, 1090, 1089, 1103, 32, 1073, 1077, 1079, 32, 1087, 1088, 1086, 1073, 1083, 1077, 1084, 44, 32, 1085, 1086, 32, 1077, 1089, 1083, 1080, 32, 1074, 1099, 32, 1086, 1090, 1082, 1088, 1086, 1077, 1090, 1077, 32, 1076, 1088, 1091, 1075, 1080, 1077, 32, 1087, 1088, 1080, 1083, 1086, 1078, 1077, 1085, 1080, 1103, 32, 1087, 1088, 1080, 32, 1088, 1072, 1073, 1086, 1090, 1072, 1102, 1097, 1077, 1081, 32, 1080, 1075, 1088, 1077, 32, 34, 68, 82, 79, 68, 34, 44, 32, 1074, 1086, 1079, 1084, 1086, 1078, 1085, 1099, 32, 1074, 1099, 1083, 1077, 1090, 1099, 46, 32, 32, 1063, 1090, 1086, 1073, 1099, 32, 1080, 1079, 1073, 1077, 1078, 1072, 1090, 1100, 32, 1090, 1072, 1082, 1086, 1081, 32, 1089, 1080, 1090, 1091, 1072, 1094, 1080, 1080, 32, 1080, 32, 1087, 1086, 1074, 1099, 1089, 1080, 1090, 1100, 32, 1087, 1088, 1086, 1080, 1079, 1074, 1086, 1076, 1080, 1090, 1077, 1083, 1100, 1085, 1086, 1089, 1090, 1100, 32, 34, 68, 82, 79, 68, 34, 44, 32, 1074, 1099, 32, 1084, 1086, 1078, 1077, 1090, 1077, 32, 1079, 1072, 1082, 1088, 1099, 1090, 1100, 32, 1086, 1089, 1090, 1072, 1083, 1100, 1085, 1099, 1077, 32, 1087, 1088, 1080, 1083, 1086, 1078, 1077, 1085, 1080, 1103, 46, 0};
		static const WCHAR wstrMemLowExitNeeded[] = {1053, 1077, 1076, 1086, 1089, 1090, 1072, 1090, 1086, 1095, 1085, 1086, 32, 1087, 1072, 1084, 1103, 1090, 1080, 32, 1076, 1083, 1103, 32, 1079, 1072, 1087, 1091, 1089, 1082, 1072, 32, 34, 68, 82, 79, 68, 34, 46, 32, 32, 1047, 1072, 1082, 1088, 1086, 1081, 1090, 1077, 32, 1086, 1090, 1082, 1088, 1099, 1090, 1099, 1077, 32, 1087, 1088, 1080, 1083, 1086, 1078, 1077, 1085, 1080, 1103, 32, 1080, 32, 1087, 1086, 1087, 1099, 1090, 1072, 1081, 1090, 1077, 1089, 1100, 32, 1079, 1072, 1087, 1091, 1089, 1090, 1080, 1090, 1100, 32, 1080, 1075, 1088, 1091, 32, 1077, 1097, 1077, 32, 1088, 1072, 1079, 46, 32, 32, 1057, 1077, 1081, 1095, 1072, 1089, 32, 1080, 1075, 1088, 1072, 32, 34, 68, 82, 79, 68, 34, 32, 1073, 1091, 1076, 1077, 1090, 32, 1079, 1072, 1082, 1088, 1099, 1090, 1072, 46, 0};
		static const WCHAR wstrAppConfigError[] = {1044, 1072, 1085, 1085, 1072, 1103, 32, 1074, 1077, 1088, 1089, 1080, 1103, 32, 1080, 1075, 1088, 1099, 32, 1085, 1077, 1076, 1077, 1081, 1089, 1090, 1074, 1080, 1090, 1077, 1083, 1100, 1085, 1072, 46, 32, 32, 1054, 1073, 1088, 1072, 1097, 1072, 1081, 1090, 1077, 1089, 1100, 32, 1074, 32, 1090, 1077, 1093, 1085, 1080, 1095, 1077, 1089, 1082, 1091, 1102, 32, 1087, 1086, 1076, 1076, 1077, 1088, 1078, 1082, 1091, 32, 1085, 1072, 32, 119, 119, 119, 46, 67, 97, 114, 97, 118, 101, 108, 71, 97, 109, 101, 115, 46, 99, 111, 109, 46, 0};
		static const WCHAR wstrAppAlreadyRunning[] = {1044, 1056, 1054, 1044, 32, 1091, 1078, 1077, 32, 1079, 1072, 1087, 1091, 1097, 1077, 1085, 46, 0};
		static const WCHAR wstrUnknownError[] = {1055, 1088, 1086, 1080, 1079, 1086, 1096, 1083, 1072, 32, 1085, 1077, 1086, 1078, 1080, 1076, 1072, 1085, 1085, 1072, 1103, 32, 1086, 1096, 1080, 1073, 1082, 1072, 44, 32, 1087, 1088, 1080, 1095, 1080, 1085, 1072, 32, 1087, 1088, 1086, 1073, 1083, 1077, 1084, 1099, 32, 1085, 1077, 32, 1091, 1089, 1090, 1072, 1085, 1086, 1074, 1083, 1077, 1085, 1072, 46, 32, 32, 1069, 1090, 1091, 32, 1087, 1088, 1086, 1073, 1083, 1077, 1084, 1091, 32, 1084, 1086, 1078, 1085, 1086, 32, 1088, 1077, 1096, 1080, 1090, 1100, 32, 1087, 1077, 1088, 1077, 1091, 1089, 1090, 1072, 1085, 1086, 1074, 1082, 1086, 1081, 32, 34, 68, 82, 79, 68, 34, 46, 32, 1054, 1096, 1080, 1073, 1082, 1072, 0};
		static const WCHAR wstrDataPathDotTextFileIsInvalid[] = {0}; //TODO
		switch (dwMessageID)
		{
			case MID_DatMissing:
				wstrMessage = wstrDatMissing;
			break;
			case MID_DatNoAccess:
				wstrMessage = wstrDatNoAccess;
			break;
			case MID_DatCorrupted_NoBackup:
				wstrMessage = wstrDatCorrupted_NoBackup;
			break;
			case MID_DatCorrupted_Restored:
				wstrMessage = wstrDatCorrupted_Restored;
			break;
			case MID_CouldNotOpenDB:
				wstrMessage = wstrCouldNotOpenDB;
			break;
			case MID_MemPerformanceWarning:
				wstrMessage = wstrMemPerformanceWarning;
			break;
			case MID_MemLowWarning:
				wstrMessage = wstrMemLowWarning;
			break;
			case MID_MemLowExitNeeded:
				wstrMessage = wstrMemLowExitNeeded;
			break;
			case MID_AppConfigError:
				wstrMessage = wstrAppConfigError;
			break;
			case MID_DRODIsAlreadyRunning:
				wstrMessage = wstrAppAlreadyRunning;
			break;
			case MID_DataPathDotTextFileIsInvalid:
				wstrMessage = wstrDataPathDotTextFileIsInvalid;
			break;
			default:
				wstrMessage = wstrUnknownError;
				WCHAR temp[10]; //error code
				wstrMessage += wszColon;
				wstrMessage += wszSpace;
				wstrMessage += _itoW(dwMessageID, temp, 10);
				ASSERT(!"Unexpected MID value."); //Probably forgot to add a MID to the database.
			break;
#else
		switch (dwMessageID)
		{
			case MID_DatMissing:
				AsciiToUnicode("Couldn't find DROD data.  This problem might be corrected by "
						"reinstalling DROD.", wstrMessage);
			break;

			case MID_DatNoAccess:
				AsciiToUnicode("Couldn't access DROD data.  If you are running DROD from a networked "
						"location, this could be a cause of the problem.  It's also possible that another "
						"application is trying to access DROD data at the same time; you may wish to "
						"retry running DROD after other applications have been closed.", wstrMessage);
			break;

			case MID_DatCorrupted_NoBackup:
				 AsciiToUnicode("Your DROD data was corrupted due to an error.  DROD tried to restore "
							"from the last good copy of the data, but the operation failed.  I recommend "
							"reinstalling DROD, but unfortunately, you will lose all your data.", wstrMessage);
			break;

			case MID_DatCorrupted_Restored:
				 AsciiToUnicode("Your DROD data was corrupted due to an error, so it was necessary to "
							"restore from the last good copy of the data. Unfortunately, you've lost saved "
							"games and other changes from your last session.", wstrMessage);
			break;

			case MID_CouldNotOpenDB:
				AsciiToUnicode("Couldn't open DROD data.  This points to corruption of a required "
						"DROD file.  This problem might be corrected by reinstalling DROD.",
						wstrMessage);
			break;

			case MID_MemPerformanceWarning:
				 AsciiToUnicode("DROD should run without any problems, but its performance may be improved "
							"by freeing memory on your system.  If there are any open applications you can "
							"close, this would help.", wstrMessage);
			break;

			case MID_MemLowWarning:
				 AsciiToUnicode("Your system is running a little low on memory.  DROD will probably run "
							"without problems, but if other applications are started while DROD is running, "
							"you might see some crashes.  To avoid this kind of thing and get better "
							"performance from DROD, you could close other applications that are now open.",
							wstrMessage);
			break;

			case MID_MemLowExitNeeded:
				 AsciiToUnicode("There is not enough memory to run DROD.  It might help to close other "
							"applications that are now open, and try running DROD again.  DROD will now exit.",
							wstrMessage);
			break;

			case MID_AppConfigError:
				 AsciiToUnicode("This version of DROD has disabled itself.  Go to www.caravelgames.com for support.",
							wstrMessage);
			break;

			case MID_DRODIsAlreadyRunning:
				AsciiToUnicode("DROD is already running.", wstrMessage);
			break;

			case MID_DataPathDotTextFileIsInvalid:
				AsciiToUnicode("The information in DataPath.txt is invalid.", wstrMessage);
			break;

			default:
			{
				AsciiToUnicode("An unexpected error occurred, and DROD was not able to retrieve a "
						"description of the problem.  This problem might be corrected by "
						"reinstalling DROD." NEWLINE
						"Error=", wstrMessage);
				WCHAR temp[16];
				wstrMessage += _itoW(dwMessageID, temp, 10);
				ASSERT(!"Unexpected MID value."); //Probably forgot to add a message to the database.
			}
			break;
#endif
		}
		pwczMessage = wstrMessage.c_str();
	}
	else
	{
		wstrMessage = pwczMessage;
	}

//Win32: UCS-2, GTK/Linux: UTF-8
#ifdef WIN32
	WSTRING wstrTitle;
#define SETTITLE(x,y) AsciiToUnicode(x, wstrTitle)
#define MSG_WARNING
#define MSG_ERROR
#else
	string strTitle;
#ifdef USE_GTK
	GtkMessageType msgtype;
#define SETTITLE(x,y) do { strTitle = x; msgtype = y; } while (0)
#define MSG_WARNING GTK_MESSAGE_WARNING
#define MSG_ERROR   GTK_MESSAGE_ERROR
#else
#define SETTITLE(x,y) strTitle = x
#define MSG_WARNING
#define MSG_ERROR
#endif
#endif

#ifdef RUSSIAN_BUILD
#ifdef USE_GTK
#error FIXME
#endif
	static const WCHAR wstrAlert[] = {1042, 1085, 1080, 1084, 1072, 1085, 1080, 1077, 0};
	static const WCHAR wstrProblemStartingDROD[] = {1055, 1088, 1086, 1073, 1083, 1077, 1084, 1072, 32, 1089, 32, 1079, 1072, 1087, 1091, 1089, 1082, 1086, 1084, 32, 34, 68, 82, 79, 68, 34, 0};
	switch (dwMessageID)
	{
		case MID_MemPerformanceWarning: case MID_MemLowWarning:
		case MID_DatCorrupted_Restored:
			wstrTitle = wstrAlert;
		break;
		default:
			wstrTitle = wstrProblemStartingDROD;
		break;
	}
#else
	switch (dwMessageID)
	{
		case MID_MemPerformanceWarning: case MID_MemLowWarning:
		case MID_DatCorrupted_Restored:
		case MID_SteamAPIInitError:
			SETTITLE("Alert", MSG_WARNING);
		break;
		default:
			SETTITLE("Problem Starting DROD", MSG_ERROR);
		break;
	}
#endif

#undef SETMESSAGE
#undef MSG_WARNING
#undef MSG_ERROR

	//Switch to windowed mode if in fullscreen.
	{
		SDL_Window *window = GetMainWindow();
		if ((SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))
				&& CScreen::bAllowWindowed)
		{
			SDL_SetWindowFullscreen(window, 0);
			CScreen::SetWindowCentered();
			PresentRect();
		}
	}

#ifdef WIN32
	MessageBoxW(NULL, pwczMessage, wstrTitle.c_str(), MB_OK | MB_ICONEXCLAMATION);
#else
	BYTE *u8msg = NULL;
	to_utf8(pwczMessage, u8msg);

#if defined(USE_GTK)
	bool bSuccess;
	if ((bSuccess = Dyn::LoadGTK()))
	{
		GtkWidget *dialog;

		if ((bSuccess = (Dyn::gtk_init_check(NULL, NULL)
			&& (dialog = Dyn::gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT,
				msgtype, GTK_BUTTONS_OK, "%s", u8msg)))))
		{
			Dyn::gtk_window_set_title(DYN_GTK_WINDOW(dialog), strTitle.c_str());
			Dyn::g_signal_connect_data(DYN_G_OBJECT(dialog), "delete_event",
				G_CALLBACK(gtkcb_delete_event), NULL, NULL, (GConnectFlags)0);
			Dyn::g_signal_connect_data(DYN_G_OBJECT(dialog), "response",
				G_CALLBACK(Dyn::gtk_widget_destroy), DYN_G_OBJECT(dialog),
				NULL, G_CONNECT_SWAPPED);
			Dyn::g_signal_connect_data(DYN_G_OBJECT(dialog), "destroy",
				G_CALLBACK(gtkcb_destroy), NULL, NULL, (GConnectFlags)0);
			Dyn::gtk_widget_show(DYN_GTK_WIDGET(dialog));
			Dyn::gtk_main();
		}
		Dyn::UnloadGTK();
	}
	if (!bSuccess)
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, strTitle.c_str(), (char*)u8msg, GetMainWindow());
#else
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, strTitle.c_str(), (char*)u8msg, GetMainWindow());
#endif

	delete[] u8msg;
#endif //not Win32
}

//*****************************************************************************
#ifdef WIN32
BOOL CALLBACK DetectDrodWindow(HWND hwnd, LPARAM lParam)
//Return: false if a DROD window is detected, else true
{
	HANDLE hProp = CFiles::WindowsCanBrowseUnicode() ?
			GetPropW(hwnd, W_APP_PROP) : GetPropA(hwnd, APP_PROP);
	if (hProp != hDrodWindowProp)
		// not a DROD window - return TRUE to continue processing windows
		return TRUE;

	// found a DROD window with the right magic number - bring it to the top.
	SetForegroundWindow(hwnd);
	WINDOWPLACEMENT wpl;
	GetWindowPlacement(hwnd, &wpl);
	wpl.showCmd = SW_RESTORE;
	SetWindowPlacement(hwnd, &wpl);

	static UINT wNumDrodWindows = 0;
	return ++wNumDrodWindows < 2;	//only one DROD window should be up at this point
}
#endif

//*****************************************************************************
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
// atexit callback
void DeleteLockFile()
{
	ASSERT(lockfile);
	unlink(lockfile);
	delete[] lockfile;
}
#endif

//*****************************************************************************
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
static bool PidIsRunning(char *pid)
{
#	if defined(__linux__)
	FILE *fp;
	char pidst[128];
	strcpy(pidst, "/proc/");
	strcpy(pidst + 6, pid);

	if ((fp = fopen(pidst, "r")))
		fclose(fp);

	return (fp || errno == EISDIR);

#	elif defined(__APPLE__) || defined(__FreeBSD__)
	// POSIX-compliant
	long nPid = strtol(pid, NULL, 10);
	if (!nPid) return true;
	return !kill(nPid, 0);
#	endif
}

static bool RaiseXWindow (UINT wid)
{
#if defined(__linux__) || defined(__FreeBSD__)
	bool bSuccess = false;
	Display *dpy = Dyn::XOpenDisplay(NULL);
	if (dpy)
	{
		//This probably won't actually raise the window (WMs have "focus
		//stealing prevention"), but it might make the task flash in the
		//taskbar, which I guess is better than nothing..
		bSuccess = (Dyn::XRaiseWindow(dpy, (Window)wid) == Success);
		Dyn::XCloseDisplay(dpy);
	}
	return bSuccess;

#else
	return false;
#endif
}
#endif //mac or linux or freebsd

//*****************************************************************************
bool IsAppAlreadyRunning()
//Returns: true if another instance of the app is already running, else false
{
#ifdef WIN32
	return !EnumWindows(DetectDrodWindow, 0);
#elif defined(__linux__) || defined __FreeBSD__ || defined(__APPLE__)
	// Use a lock file
	ASSERT(m_pFiles);	//Pre-Cond: need inited CFiles!
	const WCHAR dotpid[] = {We('.'),We('p'),We('i'),We('d'),We(0)};
	WSTRING tmp = m_pFiles->GetDatPath();
	tmp += wszSlash;
	tmp += wszDROD;
	tmp += dotpid;
	const UINT lflen = tmp.length();
	if (!(lockfile = new char[lflen + 1])) return true;
	UnicodeToAscii(tmp, lockfile);

	// Try opening an existing lockfile first
	FILE *fp = fopen(lockfile, "r");
	if (fp)
	{
		UINT i, wid = 0;
		unsigned char pid[256];
		const size_t pidlen = fread(pid, sizeof(char), 255, fp);
		pid[pidlen] = 0;
		bool bPidOk = (pid[0] && pid[0] != ':'), bWindowOk = false;
		fclose(fp);
		// Make sure this is a decimal number
		for (i = 0; i < pidlen && pid[i] != ':'; ++i)
		{
			if (pid[i] < '0' || pid[i] > '9')
			{
				bPidOk = false;
				break;
			}
		}
		if (bPidOk && pid[i] == ':')
		{
			pid[i] = 0;

			//Get window id
			for (const UINT wi = ++i; i < pidlen; ++i)
			{
				if (pid[i] == ':')
				{
					bWindowOk = (i != wi);
					break;
				}
				UINT hexit = (pid[i] > '9' ? pid[i] - 'a' + 10 : pid[i] - '0');
				if (hexit > 15 || (wid & 0xf0000000)) break;
				wid = (wid << 4) | hexit;
			}
		}
		else pid[i] = 0;
		if (bPidOk && PidIsRunning((char*)&pid[0]))
		{
			if (bWindowOk)
				RaiseXWindow(wid);
			return true;
		}

		// Corrupt lockfile or nonexistant pid; ignore it
		unlink(lockfile);
	}

	// Create a new lockfile
	char tmplockfile[lflen + 64];
	strcpy(tmplockfile, lockfile);
	sprintf(tmplockfile + lflen, "%u", getpid());
	fp = fopen(tmplockfile, "w");
	if (!fp || fwrite(tmplockfile + lflen,
			strlen(tmplockfile + lflen) * sizeof(char), 1, fp) != 1)
	{
		if (fp) fclose(fp);
		fprintf(stderr, "Couldn't create lock file, check permissions (%s).\n", tmplockfile);
		return true;
	}
	fclose(fp);

	// Atomic write operation
	if (link(tmplockfile, lockfile))
	{
		struct stat st;
		if (stat(tmplockfile, &st) || st.st_nlink != 2)
		{
			unlink(tmplockfile);
			return true;
		}
	}

	// Success
	unlink(tmplockfile);
	atexit(DeleteLockFile);
	return false;
#else
#  error Disallow running more than one instance of the app at a time.
#endif
}

//*****************************************************************************
void GetAppPath(
    const char *pszArg0,    //(in)  First command-line argument which will be used
                            //      to find application path if it is the best
                            //      available way.
    WSTRING &wstrAppPath)   //(out) App path.
{
    //
    //Try to use an O/S-specific means of getting the application path.
    //

#if defined(__linux__)
	char exepath[MAX_PATH];
	int len = readlink("/proc/self/exe", exepath, MAX_PATH - 1);
	if (len && len != -1)
	{
		exepath[len] = 0;
		UTF8ToUnicode(exepath, len, wstrAppPath);
		return;
	}
#elif defined(WIN32)
    WCHAR wszPathBuffer[MAX_PATH+1];
    if (GetModuleFileName(NULL, wszPathBuffer, MAX_PATH))
    {
        wstrAppPath = wszPathBuffer;
        return;
    }
    else //On older versions of Windows, Unicode functions fail.
    {
        char szPathBuffer[MAX_PATH+1];
        if (GetModuleFileNameA(NULL, szPathBuffer, MAX_PATH))
        {
            AsciiToUnicode(szPathBuffer, wstrAppPath);
            return;
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
	char fullPathBuffer[PATH_MAX];
	realpath(pszArg0, fullPathBuffer);
	UTF8ToUnicode(fullPathBuffer, wstrAppPath);
	return;
#endif

    //Fallback solution--use the command-line argument.
    UTF8ToUnicode(pszArg0, wstrAppPath);
}

//*****************************************************************************
#ifdef __linux__
static bool ReadProcMemValue (FILE* fp, const char* field, ULONGLONG *result)
{
	char buf[32];
	while (fscanf(fp, "%31s", buf) == 1)
		if (!strcmp(buf, field))
			return fscanf(fp, "%Lu kB", result) == 1;
	return false;
}
#endif

//*****************************************************************************
MESSAGE_ID CheckAvailableMemory()
//Checks if there is enough memory for DROD to run.
//
//Returns:
//MID_Success                   If there is plenty of memory.
//MID_MemPerformanceWarning     If there is enough memory, but performance may
//                              suffer, i.e. virtual memory is used.
//MID_MemLowWarning             There is barely enough memory.
//MID_MemLowExitNeeded          There is not enough memory to start DROD.
{
    //The amount of memory used before Init() is called.  In other words, after
    //libraries and the executable have been loaded into memory, but no code has
    //been executed that would require additional memory.  This is the time when
    //CheckAvailableMemory() is called.
#if defined(WIN32) || defined(__linux__) || defined(__APPLE__)
#  if defined __linux__
//#warning TODO: Verify this for linux. // still not verified, but we use -Werror now
#  endif
#  ifdef __APPLE__
#warning TODO: Verify this for Apple.
#  endif
   const ULONGLONG MEM_USED_BEFORE_INIT = 5000000; //5mb
#elif defined(__FreeBSD__)
   // Depending on which shared libraries are used, this can be anything from 4Mb
   // up to 7Mb on FreeBSD, so assume the worst case - Jamie
   const UINT MEM_USED_BEFORE_INIT = 7340032; // 7Mb
#else
#  error Define MEM_USED_BEFORE_INIT for this platform.
#endif

    //The estimated amount of memory DROD will need when fully loaded.  To get a good
    //estimate of this amount, you need to visit all of the screens in the application.
    //In 1.7+, screens are unloaded when not used, but in 1.6 they remain in memory.
#if defined(WIN32) || defined(__linux__) || defined(__APPLE__)
#  if defined __linux__
//#warning TODO: Verify this for linux. // still not verified, but we use -Werror now
#  endif
#  ifdef __APPLE__
#warning TODO: Verify this for Apple.
#  endif
   const ULONGLONG MEM_USED_FULLY_LOADED = 70000000; //70mb
#elif defined(__FreeBSD__)
   // Whilst on FreeBSD this figure is typically around 65Mb whilst playing the
   // game.
   const UINT MEM_USED_FULLY_LOADED = 75000000; // 75Mb
#else
#  error Define MEM_USED_FULLY_LOADED for this platform.
#endif

    //How much memory will be needed later in execution?
    const ULONGLONG MEM_STILL_NEEDED = MEM_USED_FULLY_LOADED - MEM_USED_BEFORE_INIT;

    //How much additional memory is needed to not worry about low-memory conditions
    //caused by other apps and background O/S functions.
    const ULONGLONG MEM_COMFORT_PAD = 20000000; //20mb

    //Get available virtual and physical memory.
#ifdef WIN32
    MEMORYSTATUS ms;
    GlobalMemoryStatus(&ms);
    qwAvailablePhysical = ms.dwAvailPhys;
    qwAvailableTotal = ms.dwAvailVirtual; //Includes physical + virtual-only memory.
#elif defined(__linux__)
	ULONGLONG qwAvailablePhysical, qwAvailableTotal, qwCache;
	FILE *fp = fopen("/proc/meminfo", "r");

	//Try new meminfo format first (in kb)
	if (fp && ReadProcMemValue(fp, "MemFree:", &qwAvailablePhysical)
			&& ReadProcMemValue(fp, "Cached:", &qwCache)
			&& ReadProcMemValue(fp, "SwapFree:", &qwAvailableTotal))
	{
		fclose(fp);
		qwAvailableTotal = (qwAvailableTotal + qwAvailablePhysical + qwCache) * 1024;
		qwAvailablePhysical = (qwAvailablePhysical + qwCache) * 1024;
	}
	//Fall back to old meminfo format (in bytes)
	else if (fp && (!fclose(fp) || true) && (fp = fopen("/proc/meminfo", "r"))
			&& fscanf(fp, " total: used: free: shared: buffers: cached:\n"
			"Mem: %*u %*u %Lu %*u %*u %Lu\nSwap: %*u %*u %Lu", //skip the rest
			&qwAvailablePhysical, &qwCache, &qwAvailableTotal) == 3)
	{
		fclose(fp);
		qwAvailableTotal += qwAvailablePhysical + qwCache;
		qwAvailablePhysical += qwCache;
	} else {
		if (fp) fclose(fp);
		// Not available or wrong format, cheat
		fprintf(stderr, "Warning: Couldn't get memory information, assuming it's ok.\n");
		qwAvailableTotal = qwAvailablePhysical = MEM_STILL_NEEDED + MEM_COMFORT_PAD;
	}

	// Now verify that we can actually use this
	// (linux doesn't use RLIMIT_RSS, and RLIMIT_DATA is apparently meaningless with glibc)
	struct rlimit rlim;
	if (!getrlimit(RLIMIT_AS, &rlim) && rlim.rlim_cur != RLIM_INFINITY)
	{
		qwAvailableTotal = min(qwAvailableTotal, (ULONGLONG)rlim.rlim_cur);
		qwAvailablePhysical = min(qwAvailablePhysical, (ULONGLONG)rlim.rlim_cur);
	}

#elif defined(__FreeBSD__)
	ULONGLONG qwAvailablePhysical, qwAvailableTotal;
	// Free physical memory on FreeBSD is made up of the inactive AND the free counts.
	int inactive_count, free_count;
	size_t inactive_count_size=sizeof(inactive_count);
	size_t free_count_size=sizeof(free_count);
	int pagesize;
	int error_getting_meminfo=1;
	static kvm_t *kd;
	struct kvm_swap swapinfo;
	char kvm_error_text[_POSIX2_LINE_MAX];
	struct rlimit resourceinfo;

	pagesize=getpagesize();

	if (sysctlbyname ("vm.stats.vm.v_inactive_count", &inactive_count, &inactive_count_size, NULL, 0) != -1)
	  if (sysctlbyname ("vm.stats.vm.v_free_count", &free_count, &free_count_size, NULL, 0) != -1)
	    if ( (kd = kvm_openfiles(NULL, "/dev/null", NULL, O_RDONLY, kvm_error_text)) == NULL)
	      fprintf (stderr, "kvm_openfile: %s\n", kvm_error_text);
	     else
	      if (kvm_getswapinfo(kd, &swapinfo, 1, 0) != -1)
	        {
		  kvm_close(kd);
		  qwAvailablePhysical = (inactive_count + free_count) * pagesize;
		  qwAvailableTotal = (inactive_count + free_count + swapinfo.ksw_total - swapinfo.ksw_used) * pagesize;
		  // So far we've gathered system memory figures, but we need to also check
		  // resources allocated to the users process, which could have a further
		  // restriction on memory use:
	          if (getrlimit (RLIMIT_AS, &resourceinfo) == 0)
		    {
		      error_getting_meminfo=0;
		      if (resourceinfo.rlim_cur != RLIM_INFINITY)
			{
			  if (resourceinfo.rlim_cur < (unsigned)qwAvailableTotal) qwAvailableTotal = resourceinfo.rlim_cur;
			  if (resourceinfo.rlim_cur < (unsigned)qwAvailablePhysical) qwAvailablePhysical = resourceinfo.rlim_cur;
			}
		    }
	        }

	if (error_getting_meminfo == 1)
	  {
            // Not available or wrong format, cheat
            fprintf(stderr, "Warning: Couldn't get memory information, assuming it's ok.\n");
            qwAvailableTotal = qwAvailablePhysical = MEM_STILL_NEEDED + MEM_COMFORT_PAD;
	  }

	#ifdef BETA
		fprintf (stderr, "DEBUG: qwAvailablePhysical    %d\n", qwAvailablePhysical);
		fprintf (stderr, "DEBUG: qwAvailableTotal       %d\n", qwAvailableTotal);
		fprintf (stderr, "DEBUG: MEM_USED_BEFORE_INIT   %d\n", MEM_USED_BEFORE_INIT);
		fprintf (stderr, "DEBUG: MEM_USED_FULLY_LOADED  %d\n\n", MEM_USED_FULLY_LOADED);
	#endif

#elif defined(__APPLE__)
	ULONGLONG qwAvailablePhysical, qwAvailableTotal, qwCache;
	// cheat
	qwAvailableTotal = qwAvailablePhysical = MEM_STILL_NEEDED + MEM_COMFORT_PAD;

#ifndef MEMORY_AVAILABLE_HACK  // define it to skip the heuristic checks
	// Apple's "top" program uses an undocumented function, host_statistics, to
	// capture VM information.  I instead opt to check a few values that will
	// limit the amount of memory available to the process.  However,
	// these values may still be optimistic.

	unsigned char buffer[sizeof(int)];
	size_t bufferSize = sizeof(buffer);
	int mib[] = { CTL_HW, HW_USERMEM };  // This is total physical, minus OS kernel, I believe.
	sysctl(mib, 2, buffer, &bufferSize, NULL, 0);
	int vPHYSMEM = *reinterpret_cast<int*>(buffer);
	if (vPHYSMEM > 0 && unsigned(vPHYSMEM) < qwAvailablePhysical) qwAvailablePhysical = vPHYSMEM;

	// Note that getrlimit reflects any limits on the amount of memory that can be
	// allocated to a process.  These values need not have been set to realistic
	// values that match the physical memory and swap space available on the
	// machine.  In fact typically, they are not.  On the other hand, checking
	// how much physical memory and swap space are currently unused ignores whether
	// or not this process is actually permitted to use that memory, and relies on a
	// snapshot of the system.  The checks below would be appropriate under
	// Linux in addition to the existing ones.  Also, setrlimit should be used
	// (not done here) to ensure that the limits imposed are the most permissive
	// ones permitted.  (does runtime library automatically exceed the current limits?)
	rlimit rlTotal;
	rlimit rlPhysical;
	int resultTotal = getrlimit(RLIMIT_DATA, &rlTotal);
	if (!resultTotal && rlTotal.rlim_max != -1 && rlTotal.rlim_max < qwAvailableTotal)
		qwAvailableTotal = rlTotal.rlim_max;

	int resultPhysical = getrlimit(RLIMIT_RSS, &rlPhysical);
	if (!resultPhysical && rlPhysical.rlim_max != -1 && rlPhysical.rlim_max < qwAvailablePhysical)
		qwAvailablePhysical = rlPhysical.rlim_max;

	struct statfs buf;
	int n = statfs("/private/var/vm", &buf);
		// This is the _usual_ location for swap files.  For simplicity, I will assume that
		// they reside on the same volume as the directory.
	uintmax_t bytesFree = uintmax_t(buf.f_bsize) * buf.f_bfree;
	if (!n && bytesFree < qwAvailableTotal) qwAvailableTotal = bytesFree;

#else
	fprintf(stderr, "Warning: Can't get memory information, assuming it's ok.\n");
#endif
#else
#   error Need code to get available virtual and physical memory for this platform.
#endif

	//Is there enough memory left to run DROD?
	if (qwAvailableTotal < MEM_STILL_NEEDED) //No.
		return MID_MemLowExitNeeded;

	//Yes.
	//Comfortably?
	if (qwAvailableTotal < MEM_STILL_NEEDED + MEM_COMFORT_PAD) //No.
		return MID_MemLowWarning;

	//Yes, DROD can run without fear of sudden death.
	//But will it run fast?
	if (qwAvailablePhysical < MEM_STILL_NEEDED + MEM_COMFORT_PAD) //Probably not.
		return MID_MemPerformanceWarning;

	//Probably.
	return MID_Success;
}

//*****************************************************************************
void InitCDate()
//Initialize CDate class with month name texts.
{
	 WSTRING wstrMonthNames[MONTH_COUNT];
	 const WCHAR * pwzMonthNames[MONTH_COUNT];
	 wstrMonthNames[0] = g_pTheDB->GetMessageText(MID_January);
	 wstrMonthNames[1] = g_pTheDB->GetMessageText(MID_February);
	 wstrMonthNames[2] = g_pTheDB->GetMessageText(MID_March);
	 wstrMonthNames[3] = g_pTheDB->GetMessageText(MID_April);
	 wstrMonthNames[4] = g_pTheDB->GetMessageText(MID_May);
	 wstrMonthNames[5] = g_pTheDB->GetMessageText(MID_June);
	 wstrMonthNames[6] = g_pTheDB->GetMessageText(MID_July);
	 wstrMonthNames[7] = g_pTheDB->GetMessageText(MID_August);
	 wstrMonthNames[8] = g_pTheDB->GetMessageText(MID_September);
	 wstrMonthNames[9] = g_pTheDB->GetMessageText(MID_October);
	 wstrMonthNames[10] = g_pTheDB->GetMessageText(MID_November);
	 wstrMonthNames[11] = g_pTheDB->GetMessageText(MID_December);
	 for (UINT wMonthNo = 0; wMonthNo < MONTH_COUNT; ++wMonthNo)
		  pwzMonthNames[wMonthNo] = wstrMonthNames[wMonthNo].c_str();

	 CDate::InitClass(pwzMonthNames);
}

//*****************************************************************************
void RepairMissingINIKeys(const bool bFullVersion)
//Adds default values to keys missing from the INI.
{
	string temp;
#define AddIfMissing(section, key, value) \
	if (!m_pFiles->GetGameProfileString((section), (key), temp)) \
		m_pFiles->WriteGameProfileString((section), (key), (value))

	AddIfMissing(INISection::Customizing, INIKey::AlwaysFullBlit, "0");
	AddIfMissing(INISection::Customizing, INIKey::AutoLogin, "0");
	AddIfMissing(INISection::Customizing, INIKey::CrossfadeDuration, "400");
	AddIfMissing(INISection::Customizing, INIKey::ExportSpeech, "0");
	AddIfMissing(INISection::Customizing, INIKey::FullScoreUpload, "0");
	AddIfMissing(INISection::Customizing, INIKey::LogVars, "0");
	AddIfMissing(INISection::Customizing, INIKey::MaxDelayForUndo, "500");
	AddIfMissing(INISection::Customizing, INIKey::QuickPlayerExport, "0");
	AddIfMissing(INISection::Customizing, INIKey::RoomTransitionSpeed, "500");
	AddIfMissing(INISection::Customizing, INIKey::ValidateSavesOnImport, "1");
	AddIfMissing(INISection::Customizing, INIKey::Windib, "1");

	AddIfMissing(INISection::Graphics, "Clock", "Clock");
	AddIfMissing(INISection::Graphics, "General", "GeneralTiles");
	AddIfMissing(INISection::Graphics, "Aboveground", "Aboveground");
	AddIfMissing(INISection::Graphics, "Aboveground Skies", "DayPuffyClouds;DuskClouds;SunsetRed;NightPuffyClouds;DarkNightPuffyClouds;DarkNightPuffyClouds;DarkNightPuffyClouds");
//!!TODO: update selected skies for Caldera, Greenhouse and Badlands
	AddIfMissing(INISection::Graphics, "Caldera", "Caldera");
	AddIfMissing(INISection::Graphics, "Caldera Skies", "DayEvenClouds;DuskEvenClouds;DuskEvenClouds;NightEvenClouds;DarkNightClouds;DarkNightClouds;DarkNightClouds");
	AddIfMissing(INISection::Graphics, "Beach", "Beach");
	AddIfMissing(INISection::Graphics, "Beach Skies", "DayStormy;DuskStormy;DuskStormy;NightStormy;DarkNightStormy;DarkNightStormy;DarkNightStormy");
	AddIfMissing(INISection::Graphics, "City", "City");
	AddIfMissing(INISection::Graphics, "City Skies", "DayStormy;DuskStormy;DuskStormy;NightStormy;DarkNightStormy;DarkNightStormy;DarkNightStormy");
	AddIfMissing(INISection::Graphics, "Deep Spaces", "DeepSpaces");
	AddIfMissing(INISection::Graphics, "Deep Spaces Skies", "DayStormy;DuskStormy;DuskStormy;NightStormy;DarkNightStormy;DarkNightStormy;DarkNightStormy");
	AddIfMissing(INISection::Graphics, "Fortress", "Fortress");
	AddIfMissing(INISection::Graphics, "Fortress Skies", "DayEvenClouds;DuskEvenClouds;DuskEvenClouds;NightEvenClouds;DarkNightClouds;DarkNightClouds;DarkNightClouds");
	AddIfMissing(INISection::Graphics, "Foundation", "Foundation");
	AddIfMissing(INISection::Graphics, "Foundation Skies", "DayPuffyClouds;DuskClouds;SunsetRed;NightPuffyClouds;DarkNightPuffyClouds;DarkNightPuffyClouds;DarkNightPuffyClouds");
	AddIfMissing(INISection::Graphics, "Greenhouse", "Greenhouse");
	AddIfMissing(INISection::Graphics, "Greenhouse Skies", "DayStormy;DuskStormy;DuskStormy;NightStormy;DarkNightStormy;DarkNightStormy;DarkNightStormy");
	AddIfMissing(INISection::Graphics, "Forest", "Forest");
	AddIfMissing(INISection::Graphics, "Forest Skies", "DayEvenClouds;DuskEvenClouds;DuskEvenClouds;NightEvenClouds;DarkNightClouds;DarkNightClouds;DarkNightClouds");
	AddIfMissing(INISection::Graphics, "Iceworks", "Iceworks");
	AddIfMissing(INISection::Graphics, "Iceworks Skies", "DayEvenClouds;DuskEvenClouds;DuskEvenClouds;NightEvenClouds;DarkNightClouds;DarkNightClouds;DarkNightClouds");
	AddIfMissing(INISection::Graphics, "Badlands", "Badlands");
	AddIfMissing(INISection::Graphics, "Badlands Skies", "DayPuffyClouds;DuskClouds;SunsetRed;NightPuffyClouds;DarkNightPuffyClouds;DarkNightPuffyClouds;DarkNightPuffyClouds");
	AddIfMissing(INISection::Graphics, "Swamp", "Swamp");
	AddIfMissing(INISection::Graphics, "Swamp Skies", "DayStormy;DuskStormy;DuskStormy;NightStormy;DarkNightStormy;DarkNightStormy;DarkNightStormy");
	AddIfMissing(INISection::Graphics, INIKey::Style, "Aboveground;Badlands;Beach;Caldera;City;Deep Spaces;Forest;Fortress;Foundation;Greenhouse;Iceworks;Swamp");

	AddIfMissing(INISection::Localization, INIKey::ExportText, "0");
	AddIfMissing(INISection::Localization, INIKey::Keyboard, "0");
#ifdef RUSSIAN_BUILD
	AddIfMissing(INISection::Localization, INIKey::Language, "Rus");
#else
	AddIfMissing(INISection::Localization, INIKey::Language, "Eng");
#endif

	AddIfMissing(INISection::Songs, "WinGame_TSS", "TSS/D3 song13 lis01.ogg");
	AddIfMissing(INISection::Songs, "WinGame_GATEB", "FnM/FM WIN GAME.ogg");
	AddIfMissing(INISection::Songs, "WinGame_KDD", "Construction.ogg");
	AddIfMissing(INISection::Songs, "WinGame_JTRH", "Construction.ogg");
	AddIfMissing(INISection::Songs, "WinGame_TCB", "Construction.ogg");
	AddIfMissing(INISection::Songs, "Credits_GATEB", "FnM/FM CREDITS.ogg");
	AddIfMissing(INISection::Songs, "Credits_JTRH", "SoFarBelow.ogg");
	AddIfMissing(INISection::Songs, "Credits_KDD", "SoFarBelow.ogg");
	AddIfMissing(INISection::Songs, "Credits_TCB", "credits.ogg");
	AddIfMissing(INISection::Songs, "Credits_TSS", "TSS/In a Vat lis02C.ogg");
	AddIfMissing(INISection::Songs, "Exit_TSS", "TSS/D3 song13 lis01.ogg");
	AddIfMissing(INISection::Songs, "Exit_GATEB", "FnM/FM SELL.ogg");
	AddIfMissing(INISection::Songs, "Exit_JTRH", "Busride.ogg");
	AddIfMissing(INISection::Songs, "Exit_KDD", "Busride.ogg");
	AddIfMissing(INISection::Songs, "Exit_TCB", "Busride.ogg");
	AddIfMissing(INISection::Songs, "Finale_JTRH", "Smaller Plans.ogg");
	AddIfMissing(INISection::Songs, "Finale_TCB", "win game.ogg");
	AddIfMissing(INISection::Songs, "Intro_TSS", "TSS/Subterranean Assault.ogg");
	AddIfMissing(INISection::Songs, "Intro_GATEB", "FnM/FM TITLE THEME.ogg");
	AddIfMissing(INISection::Songs, "Intro_JTRH", "TitleErikMix.ogg");
	AddIfMissing(INISection::Songs, "Intro_KDD", "Whistler.ogg");
	AddIfMissing(INISection::Songs, "Intro_TCB", "Title.ogg");

	AddIfMissing(INISection::Songs, "CityAmbient", "city ambient.ogg");
	AddIfMissing(INISection::Songs, "CityAttack", "city aggr 1.ogg;city aggr 2.ogg");
	AddIfMissing(INISection::Songs, "CityEditor", "city cont 1.ogg;city cont 2.ogg;city aggr 1.ogg;city aggr 2.ogg");
	AddIfMissing(INISection::Songs, "CityExit", "city win level.ogg");
	AddIfMissing(INISection::Songs, "CityPuzzle", "city cont 1.ogg;city cont 2.ogg");

	AddIfMissing(INISection::Songs, "Deep SpacesAmbient", "Carve it Deep.ogg");
	AddIfMissing(INISection::Songs, "Deep SpacesAttack", "In My Skin.ogg;Far Enough.ogg");
	AddIfMissing(INISection::Songs, "Deep SpacesEditor", "Uncovered.ogg;Last Gameshow.ogg;In My Skin.ogg;Far Enough.ogg");
	AddIfMissing(INISection::Songs, "Deep SpacesExit", "Devious.ogg");
	AddIfMissing(INISection::Songs, "Deep SpacesPuzzle", "Uncovered.ogg;Last Gameshow.ogg");

	AddIfMissing(INISection::Songs, "AbovegroundAmbient", "above ambient.ogg");
	AddIfMissing(INISection::Songs, "AbovegroundAttack", "above aggr 1.ogg;above aggr 2.ogg");
	AddIfMissing(INISection::Songs, "AbovegroundEditor", "above cont 1.ogg;above cont 2.ogg;above aggr 1.ogg;above aggr 2.ogg");
	AddIfMissing(INISection::Songs, "AbovegroundExit", "above win.ogg");
	AddIfMissing(INISection::Songs, "AbovegroundPuzzle", "above cont 1.ogg;above cont 2.ogg");

	if (bFullVersion)
	{
		AddIfMissing(INISection::Songs, "CalderaAmbient", "TSS/D3 song14 lis01.ogg;TSS/D3 song17 lis01.ogg");
		AddIfMissing(INISection::Songs, "CalderaAttack", "TSS/D3 Song05 lis03.ogg;TSS/D3 Song08 lis02.ogg");
		AddIfMissing(INISection::Songs, "CalderaEditor", "TSS/D3 Song01 lis03.ogg;TSS/D3 Song07 lis02.ogg;TSS/D3 song10 lis02.ogg;TSS/D3 Song05 lis03.ogg;TSS/D3 Song08 lis02.ogg");
		AddIfMissing(INISection::Songs, "CalderaExit", "TSS/D3 Song01 alt.ogg");
		AddIfMissing(INISection::Songs, "CalderaPuzzle", "TSS/D3 Song01 lis03.ogg;TSS/D3 Song07 lis02.ogg;TSS/D3 song10 lis02.ogg");
		AddIfMissing(INISection::Songs, "BeachAmbient", "FnM/CRUS NL 01.ogg;FnM/CRUS NL 02.ogg;FnM/CRUS NL 03.ogg;FnM/CRUS NL 04.ogg");
		AddIfMissing(INISection::Songs, "BeachAttack", "FnM/CRUS AG 01.ogg;FnM/CRUS AG 02.ogg;FnM/CRUS AG 03.ogg;FnM/CRUS AG 04.ogg");
		AddIfMissing(INISection::Songs, "BeachEditor", "FnM/CRUS CO 01.ogg;FnM/CRUS CO 02.ogg;FnM/CRUS CO 03.ogg;FnM/CRUS CO 04.ogg;FnM/CRUS AG 01.ogg;FnM/CRUS AG 02.ogg;FnM/CRUS AG 03.ogg;FnM/CRUS AG 04.ogg");
		AddIfMissing(INISection::Songs, "BeachExit", "above win.ogg");
		AddIfMissing(INISection::Songs, "BeachPuzzle", "FnM/CRUS CO 01.ogg;FnM/CRUS CO 02.ogg;FnM/CRUS CO 03.ogg;FnM/CRUS CO 04.ogg");
		AddIfMissing(INISection::Songs, "ForestAmbient", "FnM/ANFM NL 01.ogg;FnM/ANFM NL 02.ogg;FnM/ANFM NL 03.ogg;FnM/ANFM NL 04.ogg");
		AddIfMissing(INISection::Songs, "ForestAttack", "FnM/ANFM AG 01.ogg;FnM/ANFM AG 02.ogg;FnM/ANFM AG 03.ogg;FnM/ANFM AG 04.ogg");
		AddIfMissing(INISection::Songs, "ForestEditor", "FnM/ANFM CO 01.ogg;FnM/ANFM CO 02.ogg;FnM/ANFM CO 03.ogg;FnM/ANFM CO 04.ogg;FnM/ANFM AG 01.ogg;FnM/ANFM AG 02.ogg;FnM/ANFM AG 03.ogg;FnM/ANFM AG 04.ogg");
		AddIfMissing(INISection::Songs, "ForestExit", "city win level.ogg");
		AddIfMissing(INISection::Songs, "ForestPuzzle", "FnM/ANFM CO 01.ogg;FnM/ANFM CO 02.ogg;FnM/ANFM CO 03.ogg;FnM/ANFM CO 04.ogg");
		AddIfMissing(INISection::Songs, "FortressAmbient", "fortress ambient.ogg");
		AddIfMissing(INISection::Songs, "FortressAttack", "fortress aggr 1.ogg;fortress aggr 2.ogg");
		AddIfMissing(INISection::Songs, "FortressEditor", "fortress cont 1.ogg;fortress cont 2.ogg;fortress aggr 1.ogg;fortress aggr 2.ogg");
		AddIfMissing(INISection::Songs, "FortressExit", "fortress win.ogg");
		AddIfMissing(INISection::Songs, "FortressPuzzle", "fortress cont 1.ogg;fortress cont 2.ogg");
		AddIfMissing(INISection::Songs, "FoundationAmbient", "Sneaking around the docks.ogg");
		AddIfMissing(INISection::Songs, "FoundationAttack", "WithoutFear.ogg;Ancient Machine I.ogg");
		AddIfMissing(INISection::Songs, "FoundationEditor", "After Paraguay.ogg;Brood.ogg;WithoutFear.ogg;Ancient Machine I.ogg");
		AddIfMissing(INISection::Songs, "FoundationExit", "Delver.ogg");
		AddIfMissing(INISection::Songs, "FoundationPuzzle", "After Paraguay.ogg;Brood.ogg");
		AddIfMissing(INISection::Songs, "GreenhouseAmbient", "TSS/D3 Song02 lis03.ogg;TSS/D3 Song06 lis03.ogg");
		AddIfMissing(INISection::Songs, "GreenhouseAttack", "TSS/D3 song11 lis01.ogg;TSS/D3 song21 lis01.ogg");
		AddIfMissing(INISection::Songs, "GreenhouseEditor", "TSS/D3 song04 lis03.ogg;TSS/D3 song12 lis01.ogg;TSS/D3 song11 lis01.ogg;TSS/D3 song21 lis01.ogg");
		AddIfMissing(INISection::Songs, "GreenhouseExit", "TSS/D3 song04 alt.ogg");
		AddIfMissing(INISection::Songs, "GreenhousePuzzle", "TSS/D3 song04 lis03.ogg;TSS/D3 song12 lis01.ogg");
		AddIfMissing(INISection::Songs, "IceworksAmbient", "Leave Everything.ogg");
		AddIfMissing(INISection::Songs, "IceworksAttack", "Ive Been Here.ogg;The Steady Smite.ogg");
		AddIfMissing(INISection::Songs, "IceworksEditor", "The Reward.ogg;Larger View.ogg;Ive Been Here.ogg;The Steady Smite.ogg");
		AddIfMissing(INISection::Songs, "IceworksExit", "MySmallBox.ogg");
		AddIfMissing(INISection::Songs, "IceworksPuzzle", "The Reward.ogg;Larger View.ogg");
		AddIfMissing(INISection::Songs, "BadlandsAmbient", "TSS/D3 song16 lis01.ogg;TSS/D3 song20 lis01.ogg");
		AddIfMissing(INISection::Songs, "BadlandsAttack", "TSS/D3 song15 lis01.ogg;TSS/D3 song19 lis01.ogg");
		AddIfMissing(INISection::Songs, "BadlandsEditor", "TSS/D3 song03 lis02.ogg;TSS/D3 song18 lis01.ogg;TSS/D3 song15 lis01.ogg;TSS/D3 song19 lis01.ogg");
		AddIfMissing(INISection::Songs, "BadlandsExit", "TSS/D3 song19 alt.ogg");
		AddIfMissing(INISection::Songs, "BadlandsPuzzle", "TSS/D3 song03 lis02.ogg;TSS/D3 song18 lis01.ogg");
		AddIfMissing(INISection::Songs, "SwampAmbient", "FnM/POND NL 01.ogg;FnM/POND NL 02.ogg;FnM/POND NL 03.ogg;FnM/POND NL 04.ogg");
		AddIfMissing(INISection::Songs, "SwampAttack", "FnM/POND AG 01.ogg;FnM/POND AG 02.ogg;FnM/POND AG 03.ogg;FnM/POND AG 04.ogg");
		AddIfMissing(INISection::Songs, "SwampEditor", "FnM/POND CO 01.ogg;FnM/POND CO 02.ogg;FnM/POND CO 03.ogg;FnM/POND CO 04.ogg;FnM/POND AG 01.ogg;FnM/POND AG 02.ogg;FnM/POND AG 03.ogg;FnM/POND AG 04.ogg");
		AddIfMissing(INISection::Songs, "SwampExit", "fortress win.ogg");
		AddIfMissing(INISection::Songs, "SwampPuzzle", "FnM/POND CO 01.ogg;FnM/POND CO 02.ogg;FnM/POND CO 03.ogg;FnM/POND CO 04.ogg");
	}

	AddIfMissing(INISection::Startup, INIKey::LogErrors, "1");

	AddIfMissing(INISection::Waves, "BeethroClear", "BeethroClear1.ogg;BeethroClear2.ogg;BeethroClear3.ogg");
	AddIfMissing(INISection::Waves, "BeethroDie", "BeethroDie1.ogg;BeethroDie2.ogg");
	AddIfMissing(INISection::Waves, "BeethroHi", "my name is Beethro.ogg;what.ogg;uh huh.ogg;uh yah.ogg;i see.ogg;what you got food.ogg");
	AddIfMissing(INISection::Waves, "BeethroOof", "BeethroOof1.ogg;BeethroOof2.ogg;BeethroOof3.ogg");
	AddIfMissing(INISection::Waves, "BeethroScared", "BeethroScared1.ogg;BeethroScared2.ogg;BeethroScared3.ogg");

	AddIfMissing(INISection::Waves, "CitizenClear", "C_clear1.ogg;C_clear2.ogg;C_clear3.ogg;C_clear4.ogg");
	AddIfMissing(INISection::Waves, "CitizenDie", "C_die1.ogg;C_die2.ogg");
	AddIfMissing(INISection::Waves, "CitizenHi", "C_hi1.ogg;C_hi2.ogg;C_hi3.ogg;C_hi4.ogg;C_hi5.ogg;C_hi6.ogg;C_hi7.ogg;C_hi8.ogg;C_hi9.ogg;C_hi10.ogg");
	AddIfMissing(INISection::Waves, "CitizenOof", "C_oof1.ogg;C_oof2.ogg");
	AddIfMissing(INISection::Waves, "CitizenScared", "C_scared1.ogg;C_scared2.ogg");

	AddIfMissing(INISection::Waves, "GoblinClear", "G_clear1.ogg;G_clear2.ogg;G_clear3.ogg;G_clear4.ogg");
	AddIfMissing(INISection::Waves, "GoblinDie", "G_die1.ogg;G_die2.ogg");
	AddIfMissing(INISection::Waves, "GoblinHi", "G_hi1.ogg;G_hi2.ogg;G_hi3.ogg;G_hi4.ogg;G_hi5.ogg;G_hi6.ogg;G_hi7.ogg;G_hi8.ogg;G_hi9.ogg;G_hi10.ogg");
	AddIfMissing(INISection::Waves, "GoblinOof", "G_oof1.ogg;G_oof2.ogg");
	AddIfMissing(INISection::Waves, "GoblinScared", "G_scared1.ogg;G_scared2.ogg");

	AddIfMissing(INISection::Waves, "GunthroClear", "GB_clear1.ogg;GB_clear2.ogg;GB_clear3.ogg;GB_clear4.ogg");
	AddIfMissing(INISection::Waves, "GunthroDie", "GB_die1.ogg;GB_die2.ogg");
	AddIfMissing(INISection::Waves, "GunthroHi", "GB_hi1.ogg;GB_hi2.ogg;GB_hi3.ogg;GB_hi4.ogg;GB_hi5.ogg;GB_hi6.ogg;GB_hi7.ogg;GB_hi8.ogg;GB_hi9.ogg");
	AddIfMissing(INISection::Waves, "GunthroOof", "GB_oof1.ogg;GB_oof2.ogg;GB_oof3.ogg");
	AddIfMissing(INISection::Waves, "GunthroScared", "GB_scared1.ogg;GB_scared2.ogg;GB_scared3.ogg;GB_scared4.ogg");
	AddIfMissing(INISection::Waves, "GunthroTired", "GB_tired1.ogg;GB_tired2.ogg;GB_tired3.ogg");

	AddIfMissing(INISection::Waves, "Halph2CantOpen", "H2_cantopen1.ogg");
	AddIfMissing(INISection::Waves, "Halph2Die", "H2_die1.ogg;H2_die2.ogg");
	AddIfMissing(INISection::Waves, "Halph2Entered", "H2_entered1.ogg;H2_entered2.ogg;H2_entered3.ogg;H2_entered4.ogg;H2_entered5.ogg;H2_entered6.ogg");
	AddIfMissing(INISection::Waves, "Halph2Following", "H2_follow1.ogg;H2_follow2.ogg");
	AddIfMissing(INISection::Waves, "Halph2HurryUp", "H2_hurryUp1.ogg;H2_hurryUp2.ogg");
	AddIfMissing(INISection::Waves, "Halph2Interrupted", "H2_blocked1.ogg;H2_blocked2.ogg");
	AddIfMissing(INISection::Waves, "Halph2Striking", "H2_getDoor1.ogg;H2_getDoor2.ogg");
	AddIfMissing(INISection::Waves, "Halph2Waiting", "H2_wait1.ogg;H2_wait2.ogg");
	AddIfMissing(INISection::Waves, "Halph2Clear", "H2_clear1.ogg;H2_clear2.ogg;H2_clear3.ogg");
	AddIfMissing(INISection::Waves, "Halph2Hi", "H2_hi1.ogg;H2_hi2.ogg;H2_hi3.ogg;H2_hi4.ogg");
	AddIfMissing(INISection::Waves, "Halph2Oof", "H2_oof1.ogg;H2_oof2.ogg");
	AddIfMissing(INISection::Waves, "Halph2Scared", "H2_scared1.ogg;H2_scared2.ogg");

	AddIfMissing(INISection::Waves, "HalphCantOpen", "HalphDoorBlocked1.ogg;HalphDoorBlocked2.ogg");
	AddIfMissing(INISection::Waves, "HalphDie", "ow that hurts.ogg;hey i died.ogg");
	AddIfMissing(INISection::Waves, "HalphEntered", "heya unk.ogg;here i am again.ogg;what did i miss.ogg;im back.ogg;whats going on.ogg;oh hey there you are.ogg");
	AddIfMissing(INISection::Waves, "HalphFollowing", "HalphFollow1.ogg;HalphFollow2.ogg");
	AddIfMissing(INISection::Waves, "HalphHurryUp", "HalphHurryUp1.ogg;HalphHurryUp2.ogg");
	AddIfMissing(INISection::Waves, "HalphInterrupted", "now i cant get there.ogg;hey im blocked.ogg");
	AddIfMissing(INISection::Waves, "HalphStriking", "HalphGetDoor1.ogg;HalphGetDoor2.ogg");
	AddIfMissing(INISection::Waves, "HalphWaiting", "HalphWait1.ogg;HalphWait2.ogg");

	AddIfMissing(INISection::Waves, "MonsterClear", "hiss.ogg");
	AddIfMissing(INISection::Waves, "MonsterOof", "hiss_short.ogg");

	AddIfMissing(INISection::Waves, "NegoClear", "N_clear1.ogg;N_clear2.ogg");
	AddIfMissing(INISection::Waves, "NegoDie", "N_die1.ogg;N_die2.ogg");
	AddIfMissing(INISection::Waves, "NegoHi", "N_hi1.ogg;N_hi2.ogg;N_hi3.ogg;N_hi4.ogg;N_hi5.ogg");
	AddIfMissing(INISection::Waves, "NegoOof", "N_oof1.ogg;N_oof2.ogg");
	AddIfMissing(INISection::Waves, "NegoScared", "N_scared1.ogg;N_scared2.ogg");

	AddIfMissing(INISection::Waves, "RockClear", "RG_clear1.ogg;RG_clear2.ogg");
	AddIfMissing(INISection::Waves, "RockDie", "RG_die1.ogg;RG_die2.ogg");
	AddIfMissing(INISection::Waves, "RockHi", "RG_hi1.ogg;RG_hi2.ogg;RG_hi3.ogg");
	AddIfMissing(INISection::Waves, "RockOof", "RG_oof1.ogg;RG_oof2.ogg");
	AddIfMissing(INISection::Waves, "RockScared", "RG_scared1.ogg;RG_scared2.ogg");

	AddIfMissing(INISection::Waves, "SlayerClear", "S_clear1.ogg;S_clear2.ogg");
	AddIfMissing(INISection::Waves, "SlayerDie", "S_die.ogg");
	AddIfMissing(INISection::Waves, "SlayerHi", "S_hi1.ogg;S_hi2.ogg;S_hi3.ogg;S_hi4.ogg;S_hi5.ogg;S_hi6.ogg");
	AddIfMissing(INISection::Waves, "SlayerOof", "S_oof1.ogg;S_oof2.ogg");
	AddIfMissing(INISection::Waves, "SlayerScared", "S_scared1.ogg;S_scared2.ogg");

	AddIfMissing(INISection::Waves, "Slayer2Combat", "S2_combat1.ogg;S2_combat2.ogg;S2_combat3.ogg;S2_combat4.ogg;S2_combat5.ogg;S2_combat6.ogg;S2_combat7.ogg;S2_combat8.ogg");
	AddIfMissing(INISection::Waves, "Slayer2EnterNear", "S2_enterNear1.ogg;S2_enterNear2.ogg;S2_enterNear3.ogg;S2_enterNear4.ogg");
	AddIfMissing(INISection::Waves, "Slayer2EnterFar", "S2_enterFar1.ogg;S2_enterFar2.ogg;S2_enterFar3.ogg;S2_enterFar4.ogg");
	AddIfMissing(INISection::Waves, "Slayer2Kill", "S2_kill1.ogg;S2_kill2.ogg;S2_kill3.ogg;S2_kill4.ogg");

	AddIfMissing(INISection::Waves, "SlayerCombat", "i am unassailable.ogg;come closer and strike.ogg;cut me if you can.ogg;i will give you the hook.ogg;prepare for your removal.ogg;ah this maneuver.ogg;careful the hook is sharp.ogg;slaying time.ogg");
	AddIfMissing(INISection::Waves, "SlayerEnterFar", "keep running delver.ogg;the wisp will find you.ogg;wait there ill be along.ogg");
	AddIfMissing(INISection::Waves, "SlayerEnterNear", "ready for the hook.ogg;the empire commands.ogg;ah there you are.ogg");
	AddIfMissing(INISection::Waves, "SlayerKill", "laughing.ogg;a textbook delver mistake.ogg;finally how could i.ogg;the job is done.ogg");

	AddIfMissing(INISection::Waves, "SoldierClear", "RS_clear1.ogg;RS_clear2.ogg;RS_clear3.ogg");
	AddIfMissing(INISection::Waves, "SoldierDie", "RS_die1.ogg;RS_die2.ogg;RS_die3.ogg");
	AddIfMissing(INISection::Waves, "SoldierHi", "RS_hi1.ogg;RS_hi2.ogg");
	AddIfMissing(INISection::Waves, "SoldierOof", "RS_oof1.ogg;RS_oof2.ogg");
	AddIfMissing(INISection::Waves, "SoldierScared", "RS_scared1.ogg;RS_scared2.ogg;RS_scared3.ogg;RS_scared4.ogg;RS_scared5.ogg");

	AddIfMissing(INISection::Waves, "StalwartClear", "St_clear1.ogg;St_clear2.ogg;St_clear3.ogg");
	AddIfMissing(INISection::Waves, "StalwartDie", "St_die1.ogg;St_die2.ogg");
	AddIfMissing(INISection::Waves, "StalwartHi", "St_hi1.ogg;St_hi2.ogg;St_hi3.ogg;St_hi4.ogg;St_hi5.ogg;St_hi6.ogg;St_hi7.ogg;St_hi8.ogg");
	AddIfMissing(INISection::Waves, "StalwartOof", "St_oof1.ogg;St_oof2.ogg");
	AddIfMissing(INISection::Waves, "StalwartScared", "St_scared1.ogg;St_scared2.ogg");

	AddIfMissing(INISection::Waves, "TarOof", "TarOof.ogg");
	AddIfMissing(INISection::Waves, "TarScared", "TarScared.ogg");

	AddIfMissing(INISection::Waves, "WomanClear", "Cf_clear1.ogg;Cf_clear2.ogg;Cf_clear3.ogg");
	AddIfMissing(INISection::Waves, "WomanDie", "Cf_die1.ogg;Cf_die2.ogg");
	AddIfMissing(INISection::Waves, "WomanHi", "Cf_hi1.ogg;Cf_hi2.ogg;Cf_hi3.ogg");
	AddIfMissing(INISection::Waves, "WomanOof", "Cf_oof1.ogg;Cf_oof2.ogg");
	AddIfMissing(INISection::Waves, "WomanScared", "Cf_scared1.ogg;Cf_scared2.ogg");

	AddIfMissing(INISection::Waves, "ConstructClear", "construct_clear1.ogg;construct_clear2.ogg;construct_clear3.ogg;construct_clear4.ogg;construct_clear5.ogg");
	AddIfMissing(INISection::Waves, "ConstructDie", "construct_die1.ogg;construct_die2.ogg;construct_die3.ogg;construct_die4.ogg;construct_die5.ogg;construct_die6.ogg");
	AddIfMissing(INISection::Waves, "ConstructHi", "construct_hi0.ogg;construct_hi1.ogg;construct_hi2.ogg;construct_hi3.ogg;construct_hi4.ogg;construct_hi5.ogg;construct_hi6.ogg;construct_hi7.ogg;construct_hi8.ogg;construct_hi9.ogg;construct_hi10.ogg;construct_hi11.ogg;construct_hi12.ogg;construct_hi13.ogg;construct_hi14.ogg");
	AddIfMissing(INISection::Waves, "ConstructOof", "construct_oof1.ogg;construct_oof2.ogg;construct_oof3.ogg");
	AddIfMissing(INISection::Waves, "ConstructScared", "construct_scared1.ogg;construct_scared2.ogg");

	AddIfMissing(INISection::Waves, "EngineerClear", "eng_clear1.ogg;eng_clear2.ogg;eng_clear3.ogg;eng_clear4.ogg;eng_clear5.ogg");
	AddIfMissing(INISection::Waves, "EngineerDie", "eng_die1.ogg;eng_die2.ogg;eng_die3.ogg");
	AddIfMissing(INISection::Waves, "EngineerHi", "eng_hi1.ogg;eng_hi2.ogg;eng_hi3.ogg;eng_hi4.ogg;eng_hi5.ogg;eng_hi6.ogg;eng_hi7.ogg;eng_hi8.ogg;eng_hi9.ogg;eng_hi10.ogg;eng_hi11.ogg;eng_hi12.ogg;eng_hi13.ogg");
	AddIfMissing(INISection::Waves, "EngineerOof", "eng_oof1.ogg;eng_oof2.ogg");
	AddIfMissing(INISection::Waves, "EngineerScared", "eng_scared1.ogg;eng_scared2.ogg;eng_scared3.ogg;eng_scared4.ogg");

	AddIfMissing(INISection::Waves, "Bomb", "explosion.ogg");
	AddIfMissing(INISection::Waves, "BreakWall", "Smash_StoneWall_01.ogg;Smash_StoneWall_02.ogg");
	AddIfMissing(INISection::Waves, "Button", "buttonClick.ogg");
	AddIfMissing(INISection::Waves, "Checkpoint", "blooomp.ogg");
	AddIfMissing(INISection::Waves, "DoorOpen", "hugedoor.ogg");
	AddIfMissing(INISection::Waves, "EvilEyeWoke", "hmm.ogg");
	AddIfMissing(INISection::Waves, "Falling", "whoosh.ogg");
	AddIfMissing(INISection::Waves, "Firetrap", "firetrap.ogg");
	AddIfMissing(INISection::Waves, "FiretrapStart", "firetrap_start.ogg");
	AddIfMissing(INISection::Waves, "Frozen", "frozen.ogg");
	AddIfMissing(INISection::Waves, "Fuse", "fuselighting.ogg");
	AddIfMissing(INISection::Waves, "GolemDeath", "LowShatter.ogg");
	AddIfMissing(INISection::Waves, "GuardDeath", "NM1_Death_01.ogg;NM1_Death_02.ogg;NM1_Death_03.ogg;NM1_Death_04.ogg;NM2_Death_01.ogg;NM2_Death_02.ogg;NM2_Death_04.ogg;NM3_Death_01.ogg;NM3_Death_02.ogg;NM3_Death_03.ogg;NM3_Death_04.ogg;NM4_Death_01.ogg;NM4_Death_02.ogg;NM4_Death_04.ogg");
	AddIfMissing(INISection::Waves, "IceMelt", "icemelt.ogg");
	AddIfMissing(INISection::Waves, "LastBrain", "Powerdown2.ogg");
	AddIfMissing(INISection::Waves, "LevelComplete", "Level_Clear.ogg");
	AddIfMissing(INISection::Waves, "Mimic", "mimic.ogg");
	AddIfMissing(INISection::Waves, "NFrustrated", "NeatherMad.ogg");
	AddIfMissing(INISection::Waves, "NLaughing", "NeatherLaugh1.ogg;NeatherLaugh2.ogg;NeatherLaugh3.ogg");
	AddIfMissing(INISection::Waves, "NScared", "NeatherScared2.ogg;NeatherScared3.ogg");
	AddIfMissing(INISection::Waves, "OrbBroke", "orbbroke.ogg");
	AddIfMissing(INISection::Waves, "OrbHit", "orbhit.ogg");
	AddIfMissing(INISection::Waves, "OrbHitQuiet", "orbhitQuiet.ogg");
	AddIfMissing(INISection::Waves, "Potion", "potion.ogg");
	AddIfMissing(INISection::Waves, "PressPlate", "pressurePlate.ogg");
	AddIfMissing(INISection::Waves, "PressPlateUp", "pressurePlateUp.ogg");
	AddIfMissing(INISection::Waves, "Read", "read.ogg");
	AddIfMissing(INISection::Waves, "RoachEggSpawned", "QueenSpawn_01.ogg;QueenSpawn_02.ogg;QueenSpawn_03.ogg");
	AddIfMissing(INISection::Waves, "Run", "Boots_Leather_Run_01.ogg;Boots_Leather_Run_02.ogg;Boots_Leather_Run_03.ogg;Boots_Leather_Run_04.ogg;Boots_Leather_Run_05.ogg;Boots_Leather_Run_06.ogg;Boots_Leather_Run_07.ogg;Boots_Leather_Run_08.ogg");
	AddIfMissing(INISection::Waves, "Screenshot", "screenshot.ogg");
	AddIfMissing(INISection::Waves, "Secret", "SecretArea.ogg");
	AddIfMissing(INISection::Waves, "SeepDeath", "Seep_Kill_01.ogg;Seep_Kill_02.ogg;Seep_Kill_03.ogg;Seep_Kill_04.ogg;Seep_Kill_05.ogg;Seep_Kill_06.ogg");
	AddIfMissing(INISection::Waves, "Shatter", "shatter.ogg");
	AddIfMissing(INISection::Waves, "Sizzle", "sizzle.ogg");
	AddIfMissing(INISection::Waves, "Snoring", "snoring.ogg");
	AddIfMissing(INISection::Waves, "SoftSwing", "Blade_Swipe_01.ogg;Blade_Swipe_02.ogg;Blade_Swipe_03.ogg;Blade_Swipe_04.ogg;Blade_Swipe_05.ogg");
	AddIfMissing(INISection::Waves, "SpikesPoised", "spikes_poised.ogg");
	AddIfMissing(INISection::Waves, "SpikesUp", "spikes_up.ogg");
	AddIfMissing(INISection::Waves, "Splash", "splash.ogg");
	AddIfMissing(INISection::Waves, "Splat", "HeavyZap.ogg;HeavyZap2.ogg;HeavyZap3.ogg");
	AddIfMissing(INISection::Waves, "StabTar", "TarSplat.ogg");
	AddIfMissing(INISection::Waves, "Swing", "QuickScrape.ogg");
	AddIfMissing(INISection::Waves, "SwingLow", "QuickScrapeLow.ogg");
	AddIfMissing(INISection::Waves, "Sword", "sword1.ogg;sword2.ogg;sword3.ogg;sword4.ogg;sword5.ogg");
	AddIfMissing(INISection::Waves, "TarBabyDeath", "Squishy_Kill_01.ogg;Squishy_Kill_02.ogg");
	AddIfMissing(INISection::Waves, "TarMotherDeath", "Squishy_Kill_Low_01.ogg;Squishy_Kill_Low_01.ogg");
	AddIfMissing(INISection::Waves, "TarGrowth", "TarGrowth.ogg");
	AddIfMissing(INISection::Waves, "Tired", "BeethroTired1.ogg;BeethroTired2.ogg;BeethroTired3.ogg;BeethroTired4.ogg");
	AddIfMissing(INISection::Waves, "Trapdoor", "somethingbelow.ogg");
	AddIfMissing(INISection::Waves, "Tunnel", "poptwang.ogg");
	AddIfMissing(INISection::Waves, "Walk", "Boots_Leather_Step_01.ogg;Boots_Leather_Step_02.ogg;Boots_Leather_Step_03.ogg;Boots_Leather_Step_04.ogg;Boots_Leather_Step_05.ogg;Boots_Leather_Step_06.ogg;Boots_Leather_Step_07.ogg;Boots_Leather_Step_08.ogg");
	AddIfMissing(INISection::Waves, "WalkMonster", "monsterStep.ogg;monsterStep2.ogg");
	AddIfMissing(INISection::Waves, "Wisp", "belltoll.ogg");
	AddIfMissing(INISection::Waves, "Wubba", "wubba.ogg");
	AddIfMissing(INISection::Waves, "HornSquad", "horn_c.ogg");
	AddIfMissing(INISection::Waves, "HornSoldier", "horn_a.ogg");
	AddIfMissing(INISection::Waves, "HornFail", "horn_fail.ogg");
	AddIfMissing(INISection::Waves, "Thunder", "thunder.ogg");
	AddIfMissing(INISection::Waves, "Undo", "undo.ogg");
	AddIfMissing(INISection::Waves, "Wade", "wade.ogg");

	AddIfMissing(INISection::Waves, "BriarBreak", "briar-break1.ogg;briar-break2.ogg");
	AddIfMissing(INISection::Waves, "ChainPull", "ChainPull1.ogg;ChainPull2.ogg");
	AddIfMissing(INISection::Waves, "ConstructRevive", "ConstructRevive.ogg");
	AddIfMissing(INISection::Waves, "ConstructSmash", "ConstructSmash1.ogg;ConstructSmash2.ogg");
	AddIfMissing(INISection::Waves, "PuffExplosion", "puff-explosion.ogg");
	AddIfMissing(INISection::Waves, "SeedingBeaconOff", "SwitchOff.ogg");
	AddIfMissing(INISection::Waves, "SeedingBeaconOn", "SeedingBeacon.ogg");
	AddIfMissing(INISection::Waves, "Staff", "staff1.ogg;staff2.ogg;staff3.ogg;staff4.ogg;staff5.ogg");
	AddIfMissing(INISection::Waves, "TemporalSplitStart", "TemporalSplitStart.ogg");
	AddIfMissing(INISection::Waves, "TemporalSplitRewind", "TemporalSplitRewind.ogg");
	AddIfMissing(INISection::Waves, "Wiff", "Wiff1.ogg;Wiff2.ogg;Wiff3.ogg");
	AddIfMissing(INISection::Waves, "WiffLow", "WiffLow1.ogg;WiffLow2.ogg;WiffLow3.ogg");
	AddIfMissing(INISection::Waves, "WorldmapClick", "worldmap-click.ogg");

#undef AddIfMissing
}
