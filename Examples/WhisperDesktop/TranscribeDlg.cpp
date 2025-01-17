#include "stdafx.h"
#include "TranscribeDlg.h"
#include "Utils/logger.h"
#include <Windows.h>

HRESULT TranscribeDlg::show()
{
	auto res = DoModal( nullptr );
	if( res == -1 )
		return HRESULT_FROM_WIN32( GetLastError() );
	switch( res )
	{
	case IDC_BACK:
		return SCREEN_MODEL;
	case IDC_CAPTURE:
		return SCREEN_CAPTURE;
	}
	return S_OK;
}

constexpr int progressMaxInteger = 1024 * 8;

static const LPCTSTR regValInput = L"sourceMedia";
static const LPCTSTR regValOutFormat = L"resultFormat";
static const LPCTSTR regValOutPath = L"resultPath";
static const LPCTSTR regValUseInputFolder = L"useInputFolder";

LRESULT TranscribeDlg::OnInitDialog( UINT nMessage, WPARAM wParam, LPARAM lParam, BOOL& bHandled )
{
	// First DDX call, hooks up variables to controls.
	DoDataExchange( false );
	printModelDescription();
	languageSelector.initialize( m_hWnd, IDC_LANGUAGE, appState );
	cbConsole.initialize( m_hWnd, IDC_CONSOLE, appState );
	cbTranslate.initialize( m_hWnd, IDC_TRANSLATE, appState );
	cbDryrun.initialize( m_hWnd, IDC_DRYRUN, appState );
	populateOutputFormats();

	pendingState.initialize(
		{
			languageSelector, GetDlgItem( IDC_TRANSLATE ), GetDlgItem( IDC_DRYRUN ),
			sourceMediaPath, GetDlgItem( IDC_BROWSE_MEDIA ),
			sourceMediaFolderPath, GetDlgItem( IDC_BROWSE_MEDIA2 ),
			transcribeOutFormat, useInputFolder,
			transcribeOutputPath, GetDlgItem( IDC_BROWSE_RESULT ),
			GetDlgItem( IDCANCEL ),
			GetDlgItem( IDC_BACK ),
			GetDlgItem( IDC_CAPTURE )
		},
		{
			progressBar, GetDlgItem( IDC_PENDING_TEXT )
		} );

	HRESULT hr = work.create( this );
	if( FAILED( hr ) )
	{
		reportError( m_hWnd, L"CreateThreadpoolWork failed", nullptr, hr );
		EndDialog( IDCANCEL );
	}

	progressBar.SetRange32( 0, progressMaxInteger );
	progressBar.SetStep( 1 );

	sourceMediaPath.SetWindowText( appState.stringLoad( regValInput ) );
	transcribeOutFormat.SetCurSel( (int)appState.dwordLoad( regValOutFormat, 0 ) );
	transcribeOutputPath.SetWindowText( appState.stringLoad( regValOutPath ) );
	if( appState.boolLoad( regValUseInputFolder ) )
		useInputFolder.SetCheck( BST_CHECKED );
	BOOL unused;
	onOutFormatChange( 0, 0, nullptr, unused );

	appState.lastScreenSave( SCREEN_TRANSCRIBE );
	appState.setupIcon( this );
	ATLVERIFY( CenterWindow() );
	return 0;
}

void TranscribeDlg::printModelDescription()
{
	CString text;
	if( S_OK == appState.model->isMultilingual() )
		text = L"Multilingual";
	else
		text = L"Single-language";
	text += L" model \"";
	LPCTSTR path = appState.source.path;
	path = ::PathFindFileName( path );
	text += path;
	text += L"\", ";
	const int64_t cb = appState.source.sizeInBytes;
	if( cb < 1 << 30 )
	{
		constexpr double mul = 1.0 / ( 1 << 20 );
		double mb = (double)cb * mul;
		text.AppendFormat( L"%.1f MB", mb );
	}
	else
	{
		constexpr double mul = 1.0 / ( 1 << 30 );
		double gb = (double)cb * mul;
		text.AppendFormat( L"%.2f GB", gb );
	}
	text += L" on disk, ";
	text += implString( appState.source.impl );
	text += L" implementation";

	modelDesc.SetWindowText( text );
}

// Populate the "Output Format" combobox
void TranscribeDlg::populateOutputFormats()
{
	transcribeOutFormat.AddString( L"None" );
	transcribeOutFormat.AddString( L"Text file" );
	transcribeOutFormat.AddString( L"Text with timestamps" );
	transcribeOutFormat.AddString( L"SubRip subtitles" );
	transcribeOutFormat.AddString( L"WebVTT subtitles" );
}

// The enum values should match 0-based indices of the combobox items
enum struct TranscribeDlg::eOutputFormat : uint8_t
{
	None = 0,
	Text = 1,
	TextTimestamps = 2,
	SubRip = 3,
	WebVTT = 4,
};

enum struct TranscribeDlg::eVisualState : uint8_t
{
	Idle = 0,
	Running = 1,
	Stopping = 2
};

// CBN_SELCHANGE notification for IDC_OUTPUT_FORMAT combobox
LRESULT TranscribeDlg::onOutFormatChange( UINT, INT, HWND, BOOL& bHandled )
{
	BOOL enabled = transcribeOutFormat.GetCurSel() != 0;
	useInputFolder.EnableWindow( enabled );

	if( isChecked( useInputFolder ) && enabled )
	{
		enabled = FALSE;
		setOutputPath();
	}
	transcribeOutputPath.EnableWindow( enabled );
	transcribeOutputBrowse.EnableWindow( enabled );

	return 0;
}

// EN_CHANGE notification for IDC_PATH_MEDIA edit box
LRESULT TranscribeDlg::onInputChange( UINT, INT, HWND, BOOL& )
{
	if( !useInputFolder.IsWindowEnabled() )
		return 0;
	if( !isChecked( useInputFolder ) )
		return 0;
	setOutputPath();
	return 0;
}

void TranscribeDlg::onBrowseMedia()
{
	LPCTSTR title = L"Input audio file to transcribe";
	LPCTSTR filters = L"Multimedia Files\0*.wav;*.wave;*.mp3;*.wma;*.mp4;*.mpeg4;*.mkv;*.m4a\0\0";

	CString path;
	sourceMediaPath.GetWindowText( path );
	if( !getOpenFileName( m_hWnd, title, filters, path ) )
		return;
	sourceMediaPath.SetWindowText( path );
	if( useInputFolder.IsWindowEnabled() && useInputFolder.GetCheck() == BST_CHECKED )
		setOutputPath( path );
}
#include <ShlObj.h>

void TranscribeDlg::onBrowseMediaFolder()
{
	OutputDebugString(_T("onBrowserMediaFolder\n"));
	BROWSEINFO bi = { 0 };
	TCHAR szDisplayName[MAX_PATH];
	LPITEMIDLIST pidl;

	bi.hwndOwner = m_hWnd;
	bi.pidlRoot = NULL;
	bi.pszDisplayName = szDisplayName;
	bi.lpszTitle = _T("Select a folder");
	bi.ulFlags = BIF_RETURNONLYFSDIRS;

	pidl = SHBrowseForFolder(&bi);
	if (pidl != NULL)
	{
		if (SHGetPathFromIDList(pidl, szDisplayName))
		{
			// szDisplayName now contains the selected folder path
			// You can use it as needed
			sourceMediaPath.SetWindowText(szDisplayName);
			// Call setOutputPath if needed
			// setOutputPath(szDisplayName);
		}
		CoTaskMemFree(pidl);
	}
}

static const LPCTSTR outputFilters = L"Text files (*.txt)\0*.txt\0Text with timestamps (*.txt)\0*.txt\0SubRip subtitles (*.srt)\0*.srt\0WebVTT subtitles (*.vtt)\0*.vtt\0\0";
static const std::array<LPCTSTR, 4> outputExtensions =
{
	L".txt", L".txt", L".srt", L".vtt"
};

void TranscribeDlg::setOutputPath( const CString& input )
{
	const int format = transcribeOutFormat.GetCurSel() - 1;
	if( format < 0 || format >= outputExtensions.size() )
		return;
	const LPCTSTR ext = outputExtensions[ format ];
	CString path = input;
	path.Trim();
	const bool renamed = PathRenameExtension( path.GetBufferSetLength( path.GetLength() + 4 ), ext );
	path.ReleaseBuffer();
	if( !renamed )
		return;
	transcribeOutputPath.SetWindowText( path );
}

void TranscribeDlg::setOutputPath()
{
	CString path;
	if( !sourceMediaPath.GetWindowText( path ) )
		return;
	if( path.GetLength() <= 0 )
		return;
	setOutputPath( path );
}

void TranscribeDlg::onInputFolderCheck()
{
	const bool checked = isChecked( useInputFolder );

	BOOL enableOutput = checked ? FALSE : TRUE;
	transcribeOutputPath.EnableWindow( enableOutput );
	transcribeOutputBrowse.EnableWindow( enableOutput );

	if( !checked )
		return;
	setOutputPath();
}

void TranscribeDlg::onBrowseOutput()
{
	const DWORD origFilterIndex = (DWORD)transcribeOutFormat.GetCurSel() - 1;

	LPCTSTR title = L"Output Text File";
	CString path;
	transcribeOutputPath.GetWindowText( path );
	DWORD filterIndex = origFilterIndex;
	if( !getSaveFileName( m_hWnd, title, outputFilters, path, &filterIndex ) )
		return;

	LPCTSTR ext = PathFindExtension( path );
	if( 0 == *ext && filterIndex < outputExtensions.size() )
	{
		wchar_t* const buffer = path.GetBufferSetLength( path.GetLength() + 5 );
		PathRenameExtension( buffer, outputExtensions[ filterIndex ] );
		path.ReleaseBuffer();
	}

	transcribeOutputPath.SetWindowText( path );
	if( filterIndex != origFilterIndex )
		transcribeOutFormat.SetCurSel( filterIndex + 1 );
}

void TranscribeDlg::setPending( bool nowPending )
{
	pendingState.setPending( nowPending );
}

void TranscribeDlg::transcribeError( LPCTSTR text, HRESULT hr )
{
	reportError( m_hWnd, text, L"Unable to transcribe audio", hr );
}

CString GetFolderPath(const CString& filePath)
{
	int lastSlash = filePath.ReverseFind('\\');
	if (lastSlash == -1)
		return _T(""); // No slashes found in path

	return filePath.Left(lastSlash);
}

#include <Windows.h>
#include <vector>
#include <tchar.h>

/*
std::vector<CString> ListMP4Files(const CString& folderPath)
{
	std::vector<CString> mp4Files;
	WIN32_FIND_DATA findFileData;
	HANDLE hFind = FindFirstFile((folderPath + _T("\\*.mp4")).GetString(), &findFileData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		return mp4Files; // Return empty list
	}
	else
	{
		do
		{
			const CString filePath = folderPath + _T("\\") + findFileData.cFileName;
			mp4Files.push_back(filePath);
		} while (FindNextFile(hFind, &findFileData) != 0);
		FindClose(hFind);
	}

	return mp4Files;
}
*/

std::vector<CString> ListMediaFiles(const CString& folderPath)
{
	std::vector<CString> mediaFiles;
	WIN32_FIND_DATA findFileData;
	HANDLE hFind = INVALID_HANDLE_VALUE;

	// Define what file ext we want
	const TCHAR* extensions[] = { _T("*.mp4"), _T("*.mp3"), _T("*.mkv"), _T("*.ts") };
	const size_t extCount = sizeof(extensions) / sizeof(extensions[0]);

	// Search for each file ext
	for (size_t i = 0; i < extCount; ++i)
	{
		CString searchPath = folderPath + _T("\\") + extensions[i];
		hFind = FindFirstFile(searchPath.GetString(), &findFileData);

		if (hFind != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				{
					// Only cares about file not folder
					const CString filePath = folderPath + _T("\\") + findFileData.cFileName;
					mediaFiles.push_back(filePath);
				}
			} while (FindNextFile(hFind, &findFileData) != 0);
			FindClose(hFind);
		}
	}

	return mediaFiles;
}

CString ChangeFileExtensionToSrt(const CString& filePath)
{
	int dotIndex = filePath.ReverseFind('.');
	if (dotIndex != -1)  // Found "."
	{
		CString newPath = filePath.Left(dotIndex);  // get Main filename
		newPath += _T(".srt");  // Add "srt" at the end
		return newPath;
	}
	else  // No "."
	{
		return filePath + _T(".srt");  // Add ".srt" at the end
	}
}

VOID CALLBACK MyTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	OutputDebugString(_T("TimerProc\n"));
	KillTimer(hwnd, idEvent);
} 

struct ThreadParams {
	CString msg;
};

#include <windows.h>
#include <winhttp.h> 
#pragma comment(lib, "winhttp.lib")

void SendWebhookNotification(CString message) {
	HINTERNET hSession = WinHttpOpen(L"Webhook Sender/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);

	if (!hSession) return;

	HINTERNET hConnect = WinHttpConnect(hSession, L"n8ntest.squarebbg.com",
		INTERNET_DEFAULT_HTTPS_PORT, 0);

	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return;
	}

	CString debugMessage;
	debugMessage.Format(_T("/webhook/notifyme?msg=%s\n"), message);
	OutputDebugString(debugMessage);

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
		//L"/webhook/notifyme?msg=sendbyvisualstudio",
		debugMessage,
		NULL, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE);

	if (!hRequest) {
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return;
	}

	BOOL bResults = WinHttpSendRequest(hRequest,
		WINHTTP_NO_ADDITIONAL_HEADERS, 0,
		WINHTTP_NO_REQUEST_DATA, 0,
		0, 0);

	if (bResults) {
		bResults = WinHttpReceiveResponse(hRequest, NULL);
	}

	// Cleanup
	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
}

DWORD WINAPI ThreadProc(LPVOID lpParameter) {
	ThreadParams* params = (ThreadParams*)lpParameter;
	SendWebhookNotification(params->msg);
	delete params;
	return 0;
} 

HANDLE StartWebhookThread(const CString& msg) {
	ThreadParams* params = new ThreadParams;
	params->msg = msg;

	HANDLE hThread = CreateThread(
		NULL,                   // default security attributes
		0,                      // use default stack size  
		ThreadProc,             // thread function name
		params,                 // argument to thread function 
		0,                      // use default creation flags 
		NULL);                  // returns the thread identifier 

	if (hThread == NULL) {
		delete params;
	}

	return hThread;
}

void TranscribeDlg::onTranscribe()
{
	switch( transcribeArgs.visualState )
	{
	case eVisualState::Running:
		transcribeArgs.visualState = eVisualState::Stopping;
		transcribeButton.EnableWindow( FALSE );
		return;
	case eVisualState::Stopping:
		return;
	}

	// Validate input
	sourceMediaPath.GetWindowText( transcribeArgs.pathMedia );
	if( transcribeArgs.pathMedia.GetLength() <= 0 )
	{
		transcribeError( L"Please select an input audio file" );
		return;
	}

	if( !PathFileExists( transcribeArgs.pathMedia ) )
	{
		transcribeError( L"Input audio file does not exist", HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ) );
		return;
	}

	CString message = L"Selected File Path: " + transcribeArgs.pathMedia + _T("\n");
	OutputDebugString(message.GetString());

	BOOL isFolder = TRUE;

	// Folder or File?
	{
		const wchar_t* pathToCheck = transcribeArgs.pathMedia;

		DWORD attributes = GetFileAttributes(pathToCheck);

		if (attributes == INVALID_FILE_ATTRIBUTES) {
			OutputDebugString(L"Unable to get attribute, Error code:");
			return ;
		}

		if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
			OutputDebugString(L"It is a folder\n");
		}
		else {
			OutputDebugString(L"It is a file\n");
			isFolder = FALSE;
		}
	}

	if (isFolder)
	{
		CString folderPath = GetFolderPath(transcribeArgs.pathMedia); 
		//transcribeArgs.inputPathMediaList = ListMP4Files(transcribeArgs.pathMedia);
		transcribeArgs.inputPathMediaList = ListMediaFiles(transcribeArgs.pathMedia);
		
	}
	else
	{
		transcribeArgs.inputPathMediaList.push_back(transcribeArgs.pathMedia);
	}

	CString strOutput;
	strOutput.Format(_T("We have %d files in inputPathMediaList"), transcribeArgs.inputPathMediaList.size());
	OutputDebugString(strOutput);

#if 1
	int fileNumber = 1;

	auto it = transcribeArgs.inputPathMediaList.begin();
	while (it != transcribeArgs.inputPathMediaList.end()) {
		CString inputFilePath = *it;
		CString pathOutput = ChangeFileExtensionToSrt(inputFilePath);

		// Check if file exists
		if (PathFileExists(pathOutput)) {
			OutputDebugString(_T("file existed\n"));
			// IF exists, remove the path from vector
			it = transcribeArgs.inputPathMediaList.erase(it);
		}
		else { 
			CString debugMessage;
			debugMessage.Format(_T("File #%d\ninput file\n%s\noutput file\n%s\n"), fileNumber, inputFilePath.GetString(), pathOutput.GetString());
			OutputDebugString(debugMessage);

			// Update file number and go to next file
			fileNumber++;
			++it;
		}
	}
#else
	int no = 1;
	for (const auto& inputFilePath : transcribeArgs.inputPathMediaList)
	{ 
		CString pathOutput = ChangeFileExtensionToSrt(inputFilePath);

		CString debugMessage;

		debugMessage.Format(_T("%d\n Input file: %s\nOutput file: %s\n\n"), no++, inputFilePath.GetString(), pathOutput.GetString());

		OutputDebugString(debugMessage); 
	}
#endif
	
	transcribeArgs.language = languageSelector.selectedLanguage();
	transcribeArgs.translate = cbTranslate.checked();
	transcribeArgs.dryrun = cbDryrun.checked();
 
	if (transcribeArgs.dryrun)
	{
		OutputDebugString(_T("Dryrun Mode\n"));
		return;
	}

	if( isInvalidTranslate( m_hWnd, transcribeArgs.language, transcribeArgs.translate ) )
		return;

	transcribeArgs.format = (eOutputFormat)(uint8_t)transcribeOutFormat.GetCurSel();

	if (transcribeArgs.format != eOutputFormat::None)
	{
		appState.stringStore( regValOutPath,  transcribeArgs.pathOutput);
	}
	else
		cbConsole.ensureChecked();

	appState.dwordStore(regValOutFormat, (uint32_t)(int)transcribeArgs.format);
	appState.boolStore(regValUseInputFolder, isChecked(useInputFolder));
	languageSelector.saveSelection(appState);
	cbTranslate.saveSelection(appState);
	cbDryrun.saveSelection(appState);
	appState.stringStore(regValInput, transcribeArgs.pathMedia);

	setPending(true);
	transcribeArgs.visualState = eVisualState::Running;
	transcribeButton.SetWindowText(L"Stop");
	work.post(); 

	return;
}

void __stdcall TranscribeDlg::poolCallback() noexcept
{
	HRESULT hr = transcribe();
	PostMessage( WM_CALLBACK_STATUS, (WPARAM)hr );
}

static void printTime( CString& rdi, int64_t ticks )
{
	const Whisper::sTimeSpan ts{ (uint64_t)ticks };
	const Whisper::sTimeSpanFields fields = ts;

	if( fields.days != 0 )
	{
		rdi.AppendFormat( L"%i days, %i hours", fields.days, (int)fields.hours );
		return;
	}
	if( ( fields.hours | fields.minutes ) != 0 )
	{
		rdi.AppendFormat( L"%02d:%02d:%02d", (int)fields.hours, (int)fields.minutes, (int)fields.seconds );
		return;
	}
	rdi.AppendFormat( L"%.3f seconds", (double)ticks / 1E7 );
}

LRESULT TranscribeDlg::onCallbackStatus( UINT, WPARAM wParam, LPARAM, BOOL& bHandled )
{
	setPending( false );
	transcribeButton.SetWindowText( L"Transcribe" );
	transcribeButton.EnableWindow( TRUE );
	const bool prematurely = ( transcribeArgs.visualState == eVisualState::Stopping );
	transcribeArgs.visualState = eVisualState::Idle;

	const HRESULT hr = (HRESULT)wParam;
	if( FAILED( hr ) )
	{
		LPCTSTR failMessage = L"Transcribe failed";

		if( transcribeArgs.errorMessage.GetLength() > 0 )
		{
			CString tmp = failMessage;
			tmp += L"\n";
			tmp += transcribeArgs.errorMessage;
			transcribeError( tmp, hr );
		}
		else
			transcribeError( failMessage, hr );

		return 0;
	}

	const int64_t elapsed = ( GetTickCount64() - transcribeArgs.startTime ) * 10'000;
	const int64_t media = transcribeArgs.mediaDuration;
	CString message;
	if( prematurely )
		message = L"Transcribed an initial portion of the audio";
	else
		message = L"Transcribed the audio";
	message += L"\nMedia duration: ";
	printTime( message, media );
	message += L"\nProcessing time: ";
	printTime( message, elapsed );
	message += L"\nRelative processing speed: ";
	double mul = (double)media / (double)elapsed;
	message.AppendFormat( L"%g", mul );

	MessageBox( message, L"Transcribe Completed", MB_OK | MB_ICONINFORMATION );
	return 0;
}

void TranscribeDlg::getThreadError()
{
	getLastError( transcribeArgs.errorMessage );
}

#define CHECK_EX( hr ) { const HRESULT __hr = ( hr ); if( FAILED( __hr ) ) { getThreadError(); return __hr; } }

HRESULT TranscribeDlg::transcribe()
{
	transcribeArgs.startTime = GetTickCount64();
	clearLastError();
	transcribeArgs.errorMessage = L"";
	
	for (const auto& inputFilePath : transcribeArgs.inputPathMediaList)
	{
		CString pathOutput = ChangeFileExtensionToSrt(inputFilePath); 
		CString msgToSent;
		msgToSent.Format(_T("Start to process filename: %s\n"), inputFilePath);
		StartWebhookThread(msgToSent);

		using namespace Whisper;
		CComPtr<iAudioReader> reader;

		CHECK_EX( appState.mediaFoundation->openAudioFile( inputFilePath, false, &reader ) );

		const eOutputFormat format = transcribeArgs.format;
		CAtlFile outputFile;
		if( format != eOutputFormat::None )
			CHECK( outputFile.Create( pathOutput, GENERIC_WRITE, 0, CREATE_ALWAYS ) );

		transcribeArgs.resultFlags = eResultFlags::Timestamps | eResultFlags::Tokens;

		CComPtr<iContext> context;
		CHECK_EX( appState.model->createContext( &context ) );

		sFullParams fullParams;
		CHECK_EX( context->fullDefaultParams( eSamplingStrategy::Greedy, &fullParams ) );
		fullParams.language = transcribeArgs.language;
		fullParams.setFlag( eFullParamsFlags::Translate, transcribeArgs.translate );
		fullParams.resetFlag( eFullParamsFlags::PrintRealtime );

		// Setup the callbacks
		fullParams.new_segment_callback = &newSegmentCallbackStatic;
		fullParams.new_segment_callback_user_data = this;
		fullParams.encoder_begin_callback = &encoderBeginCallback;
		fullParams.encoder_begin_callback_user_data = this;

		// Setup the progress indication sink
		sProgressSink progressSink{ &progressCallbackStatic, this };
		// Run the transcribe
		CHECK_EX( context->runStreamed( fullParams, progressSink, reader ) );

		// Once finished, query duration of the audio.
		// The duration before the processing is sometimes different, by 20 seconds for the file in that issue:
		// https://github.com/Const-me/Whisper/issues/4
		CHECK_EX( reader->getDuration( transcribeArgs.mediaDuration ) );

		context->timingsPrint();

		if( format == eOutputFormat::None )
			return S_OK;

		CComPtr<iTranscribeResult> result;
		CHECK_EX( context->getResults( transcribeArgs.resultFlags, &result ) );

		sTranscribeLength len;
		CHECK_EX( result->getSize( len ) );
		const sSegment* const segments = result->getSegments();

		switch( format )
		{
		case eOutputFormat::Text:
			writeTextFile( segments, len.countSegments, outputFile, false );
			break;
		case eOutputFormat::TextTimestamps:
			writeTextFile( segments, len.countSegments, outputFile, true );
			break;
		case eOutputFormat::SubRip:
			writeSubRip( segments, len.countSegments, outputFile );
			break;
		case eOutputFormat::WebVTT:
			writeWebVTT( segments, len.countSegments, outputFile );
			break;
		default:
			return E_FAIL;
		}

		//CString msgToSent;
		msgToSent.Format(_T("Finish to process %s\n"), inputFilePath);
		StartWebhookThread(msgToSent);
	}

	return S_OK;
}

#undef CHECK_EX

inline HRESULT TranscribeDlg::progressCallback( double p ) noexcept
{
	constexpr double mul = progressMaxInteger;
	int pos = lround( mul * p );
	progressBar.PostMessage( PBM_SETPOS, pos, 0 );
	return S_OK;
}

HRESULT __cdecl TranscribeDlg::progressCallbackStatic( double p, Whisper::iContext* ctx, void* pv ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)pv;
	return dlg->progressCallback( p );
}

namespace
{
	HRESULT write( CAtlFile& file, const CStringA& line )
	{
		if( line.GetLength() > 0 )
			CHECK( file.Write( cstr( line ), (DWORD)line.GetLength() ) );
		return S_OK;
	}

	const char* skipBlank( const char* rsi )
	{
		while( true )
		{
			const char c = *rsi;
			if( c == ' ' || c == '\t' )
			{
				rsi++;
				continue;
			}
			return rsi;
		}
	}
}

using Whisper::sSegment;


HRESULT TranscribeDlg::writeTextFile( const sSegment* const segments, const size_t length, CAtlFile& file, bool timestamps )
{
	using namespace Whisper;
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];

		if( timestamps )
		{
			line = "[";
			printTime( line, seg.time.begin );
			line += " --> ";
			printTime( line, seg.time.end );
			line += "]  ";
		}
		else
			line = "";

		line += skipBlank( seg.text );
		line += "\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeSubRip( const sSegment* const segments, const size_t length, CAtlFile& file )
{
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];

		line.Format( "%zu\r\n", i + 1 );
		printTime( line, seg.time.begin, true );
		line += " --> ";
		printTime( line, seg.time.end, true );
		line += "\r\n";
		line += skipBlank( seg.text );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeWebVTT( const sSegment* const segments, const size_t length, CAtlFile& file )
{
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	line = "WEBVTT\r\n\r\n";
	CHECK( write( file, line ) );

	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];
		line = "";

		printTime( line, seg.time.begin, false );
		line += " --> ";
		printTime( line, seg.time.end, false );
		line += "\r\n";
		line += skipBlank( seg.text );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

inline HRESULT TranscribeDlg::newSegmentCallback( Whisper::iContext* ctx, uint32_t n_new )
{
	using namespace Whisper;
	CComPtr<iTranscribeResult> result;
	CHECK( ctx->getResults( transcribeArgs.resultFlags, &result ) );
	return logNewSegments( result, n_new );
}

HRESULT __cdecl TranscribeDlg::newSegmentCallbackStatic( Whisper::iContext* ctx, uint32_t n_new, void* user_data ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)user_data;
	return dlg->newSegmentCallback( ctx, n_new );
}

HRESULT __cdecl TranscribeDlg::encoderBeginCallback( Whisper::iContext* ctx, void* user_data ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)user_data;
	const eVisualState visualState = dlg->transcribeArgs.visualState;
	switch( visualState )
	{
	case eVisualState::Idle:
		return E_NOT_VALID_STATE;
	case eVisualState::Running:
		return S_OK;
	case eVisualState::Stopping:
		return S_FALSE;
	default:
		return E_UNEXPECTED;
	}
}

void TranscribeDlg::onWmClose()
{
	if( GetDlgItem( IDCANCEL ).IsWindowEnabled() )
	{
		EndDialog( IDCANCEL );
		return;
	}

	constexpr UINT flags = MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2;
	const int res = this->MessageBox( L"Transcribe is in progress.\nDo you want to quit anyway?", L"Confirm exit", flags );
	if( res != IDYES )
		return;

	// TODO: instead of ExitProcess(), implement another callback in the DLL API, for proper cancellation of the background task
	ExitProcess( 1 );
}