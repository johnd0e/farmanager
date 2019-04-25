﻿/*
viewer.cpp

Internal viewer
*/
/*
Copyright © 1996 Eugene Roshal
Copyright © 2000 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "viewer.hpp"

#include "encoding.hpp"
#include "macroopcode.hpp"
#include "keyboard.hpp"
#include "flink.hpp"
#include "farcolor.hpp"
#include "keys.hpp"
#include "help.hpp"
#include "dialog.hpp"
#include "filepanels.hpp"
#include "fileview.hpp"
#include "ctrlobj.hpp"
#include "scrbuf.hpp"
#include "TPreRedrawFunc.hpp"
#include "syslog.hpp"
#include "taskbar.hpp"
#include "cddrv.hpp"
#include "drivemix.hpp"
#include "interf.hpp"
#include "message.hpp"
#include "clipboard.hpp"
#include "delete.hpp"
#include "pathmix.hpp"
#include "filestr.hpp"
#include "mix.hpp"
#include "console.hpp"
#include "RegExp.hpp"
#include "colormix.hpp"
#include "vmenu.hpp"
#include "vmenu2.hpp"
#include "plugins.hpp"
#include "manager.hpp"
#include "lang.hpp"
#include "datetime.hpp"
#include "keybar.hpp"
#include "stddlg.hpp"
#include "strmix.hpp"
#include "string_utils.hpp"
#include "cvtname.hpp"
#include "panel.hpp"
#include "global.hpp"
#include "DlgGuid.hpp"

#include "platform.fs.hpp"

#include "common/bytes_view.hpp"

#include "format.hpp"

enum SHOW_MODES
{
	SHOW_RELOAD,
	SHOW_HEX,
	SHOW_UP,
	SHOW_DOWN,
	SHOW_DUMP
};

enum saved_modes
{
	m_none            = 0,

	m_mode_changed    = 0x10,

	m_mode_text       = VMT_TEXT,
	m_mode_hex        = VMT_HEX,
	m_mode_dump       = VMT_DUMP,

	m_mode_last       = m_mode_dump,
	m_mode_mask       = 0xF,

	m_mode_wrap       = 0x20,
	m_mode_wrap_words = 0x40,
};


static void PR_ViewerSearchMsg();
static void ViewerSearchMsg(const string& name, int percent, int search_hex);

static int ViewerID=0;

static bool IsCodePageSupported(uintptr_t cp)
{
	return codepages::IsCodePageSupported(cp, 2);
}

// seems like this initialization list is toooooo long
Viewer::Viewer(window_ptr Owner, bool bQuickView, uintptr_t aCodePage):
	SimpleScreenObject(std::move(Owner)),
	ViOpt(Global->Opt->ViOpt),
	Signature(),
	m_ViewKeyBar(),
	Reader(ViewFile, (Global->Opt->ViOpt.MaxLineSize*2*64 > 64*1024 ? Global->Opt->ViOpt.MaxLineSize*2*64 : 64*1024)),
	m_DeleteFolder(true),
	strLastSearchStr(Global->GetSearchString()),
	LastSearchCase(Global->GlobalSearchCase),
	LastSearchWholeWords(Global->GlobalSearchWholeWords),
	LastSearchReverse(Global->GlobalSearchReverse),
	LastSearchHex(Global->GetSearchHex()),
	LastSearchRegexp(Global->Opt->ViOpt.SearchRegexp),
	LastSearchDirection(Global->GlobalSearchReverse? -1 : +1),
	StartSearchPos(),
	m_DefCodepage(aCodePage),
	m_Codepage(m_DefCodepage),
	m_Wrap(Global->Opt->ViOpt.ViewerIsWrap),
	m_WordWrap(Global->Opt->ViOpt.ViewerWrap),
	m_DisplayMode(VMT_TEXT),
	m_DumpTextMode(),
	FilePos(),
	SecondPos(),
	FileSize(),
	LastSelectPos(),
	LastSelectSize(-1),
	LeftPos(),
	LastPage(),
	SelectPos(),
	SelectSize(-1),
	ManualSelectPos(-1),
	SelectFlags(),
	ShowStatusLine(true),
	m_HideCursor(true),
	ReadStdin(),
	InternalKey(),
	LastKeyUndo(),
	Width(),
	XX2(),
	ViewerID(::ViewerID++),
	OpenFailed(),
	bVE_READ_Sent(),
	HostFileViewer(),
	AdjustSelPosition(),
	redraw_selection(),
	m_bQuickView(bQuickView),
	m_IdleCheck(std::make_unique<time_check>(time_check::mode::delayed, 500ms)),
	vread_buffer(std::max(MaxViewLineBufferSize(), size_t(8192))),
	lcache_first(-1),
	lcache_last(-1),
	lcache_lines(16*1000),
	lcache_count(),
	lcache_base(),
	lcache_ready(),
	lcache_wrap(-1),
	lcache_wwrap(-1),
	lcache_width(-1),
	// dirty magic numbers, fix them!
	max_backward_size(std::min(Options::ViewerOptions::eMaxLineSize*3ll, std::max(Global->Opt->ViOpt.MaxLineSize*2, 1024ll) * 32)),
	llengths(max_backward_size / 40),
	Search_buffer(3 * std::max(MaxViewLineBufferSize(), size_t(8192))),
	vString(),
	vgetc_buffer(),
	vgetc_ready(),
	vgetc_cb(),
	vgetc_ib(),
	vgetc_composite(L'\0'),
	ReadBuffer(MaxViewLineBufferSize()),
	f8cps(true)
{
	if (m_DefCodepage != CP_DEFAULT)
		MB.SetCP(m_DefCodepage);
}

Viewer::~Viewer()
{
	KeepInitParameters();

	if (ViewFile)
	{
		ViewFile.Close();
		SavePosition();
	}

	_tran(SysLog(L"[%p] Viewer::~Viewer, TempViewName=[%s]",this,TempViewName));
	/* $ 11.10.2001 IS
	   Удаляем файл только, если нет открытых окон с таким именем.
	*/

	if (!strTempViewName.empty() && !Global->WindowManager->CountWindowsWithName(strTempViewName))
	{
		/* $ 14.06.2002 IS
		   Если DeleteFolder сброшен, то удаляем только файл. Иначе - удаляем еще
		   и каталог.
		*/
		if (m_DeleteFolder)
			DeleteFileWithFolder(strTempViewName);
		else
		{
			os::fs::set_file_attributes(strTempViewName,FILE_ATTRIBUTE_NORMAL);
			os::fs::delete_file(strTempViewName); //BUGBUG
		}
	}
}

wchar_t Viewer::ZeroChar() const
{
	return ViOpt.Visible0x00 && ViOpt.ZeroChar > 0 ? static_cast<wchar_t>(ViOpt.ZeroChar) : L' ';
}

struct Viewer::ViewerUndoData
{
	ViewerUndoData(long long UndoAddr, long long UndoLeft):
		UndoAddr(UndoAddr),
		UndoLeft(UndoLeft)
	{
	}
	long long UndoAddr;
	long long UndoLeft;
};

void Viewer::SavePosition()
{
	const auto& vo = Global->Opt->ViOpt;
	if (vo.SaveShortPos || vo.SavePos || vo.SaveCodepage || vo.SaveViewMode || vo.SaveWrapMode)
	{
		ViewerPosCache poscache;

		poscache.cur.FilePos = FilePos;
		poscache.cur.LeftPos = LeftPos;

		poscache.ViewModeAndWrapState = (m_DisplayMode.touched() || m_Wrap.touched() || m_WordWrap.touched())
			? m_mode_changed | m_DisplayMode | (m_Wrap ? m_mode_wrap : 0) | (m_WordWrap ? m_mode_wrap_words : 0)
			: m_none;

		poscache.CodePage = m_Codepage;
		poscache.bm = BMSavePos;

		string strCacheName = strPluginData.empty() ? strFullFileName : strPluginData+PointToName(strFileName);
		FilePositionCache::AddPosition(strCacheName, poscache);
	}
}

void Viewer::KeepInitParameters() const
{
	Global->StoreSearchString(strLastSearchStr, LastSearchHex);
	Global->GlobalSearchCase=LastSearchCase;
	Global->GlobalSearchWholeWords=LastSearchWholeWords;
	Global->GlobalSearchReverse=LastSearchReverse;
	Global->Opt->ViOpt.ViewerIsWrap = m_Wrap;
	Global->Opt->ViOpt.ViewerWrap = m_WordWrap;
	Global->Opt->ViOpt.SearchRegexp=LastSearchRegexp;
}

bool Viewer::OpenFile(const string& Name,int warning)
{
	m_Codepage=m_DefCodepage;
	m_DefCodepage=CP_DEFAULT;
	OpenFailed=false;

	ViewFile.Close();
	Reader.Clear();
	vgetc_ready = lcache_ready = false;

	SelectSize = -1; // Сбросим выделение
	strFileName = Name;

	const auto& vo = Global->Opt->ViOpt;

	if (Global->OnlyEditorViewerUsed && strFileName == L"-"sv)
	{
		const auto strTempName = MakeTemp();
		if (!ViewFile.Open(strTempName,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE))
		{
			OpenFailed = true;
			return false;
		}

		DWORD ReadSize = 0;
		while (ReadFile(console.GetOriginalInputHandle(),vread_buffer.data(),(DWORD)vread_buffer.size(),&ReadSize,nullptr) && ReadSize)
		{
			// BUGBUG check result
			(void)ViewFile.Write(vread_buffer.data(), ReadSize);
		}
		ViewFile.SetPointer(0, nullptr, FILE_BEGIN);

		//after reading from the pipe, redirect stdin to the real console stdin
		SetStdHandle(STD_INPUT_HANDLE, os::fs::low::create_file(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
		ReadStdin=TRUE;
	}
	else
	{
		for (;;)
		{
			if (ViewFile.Open(strFileName, FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING))
				break;

			/* $ 04.07.2000 tran
			   + 'warning' flag processing, in QuickView it is FALSE
				 so don't show red message box */
			if (warning)
			{
				const auto ErrorState = error_state::fetch();

				if (OperationFailed(ErrorState, strFileName, lng::MViewerTitle, msg(lng::MViewerCannotOpenFile), false) == operation::retry)
					continue;
			}

			OpenFailed = true;
			return false;
		}
	}

	Reader.AdjustAlignment();

	strFullFileName = ConvertNameToFull(strFileName);
	// BUGBUG check result
	(void)os::fs::get_find_data(strFileName, ViewFindData);
	uintptr_t CachedCodePage=0;

	if ((vo.SavePos || vo.SaveShortPos || vo.SaveCodepage || vo.SaveViewMode || vo.SaveWrapMode) && !ReadStdin)
	{
		string strCacheName = strPluginData.empty() ? strFileName : strPluginData + PointToName(strFileName);
		ViewerPosCache poscache;
		bool found = FilePositionCache::GetPosition(strCacheName, poscache);
		if (found)
		{
			if (vo.SavePos)
			{
				LastSelectPos = FilePos = std::max(poscache.cur.FilePos, 0LL);
				LeftPos = poscache.cur.LeftPos;
			}
			if (vo.SaveCodepage || vo.SavePos)
			{
				CachedCodePage = poscache.CodePage;
				if (CachedCodePage && !IsCodePageSupported(CachedCodePage))
					CachedCodePage = 0;
			}

			if (vo.SaveShortPos)
			{
				BMSavePos = poscache.bm;
			}

			if (!m_DisplayMode.touched()) // keep Mode if file listed (Gray+-)
			{
				if (vo.SaveViewMode && (poscache.ViewModeAndWrapState & m_mode_changed) != 0)
				{
					auto ViewMode = poscache.ViewModeAndWrapState & m_mode_mask;
					if (ViewMode <= m_mode_last)
					{
						m_DisplayMode = static_cast<VIEWER_MODE_TYPE>(ViewMode);
					}
				}
				if (m_DisplayMode != VMT_HEX)
					m_DumpTextMode = (m_DisplayMode == VMT_DUMP);

				if (vo.SaveWrapMode && (poscache.ViewModeAndWrapState & m_mode_changed) != 0)
				{
					m_Wrap = (poscache.ViewModeAndWrapState & m_mode_wrap) != 0;
					m_WordWrap = (poscache.ViewModeAndWrapState & m_mode_wrap_words) != 0;
				}
			}
		}
	}

	if (m_Codepage == CP_DEFAULT)
	{
		if (CachedCodePage)
		{
			m_Codepage = CachedCodePage;
		}
		else
		{
			const auto DefaultCodepage = GetDefaultCodePage();
			const auto DetectedCodepage = GetFileCodepage(ViewFile, DefaultCodepage, &Signature, vo.AutoDetectCodePage);
			m_Codepage = IsCodePageSupported(DetectedCodepage)? DetectedCodepage : DefaultCodepage;
		}

		MB.SetCP(m_Codepage);
	}

	ViewFile.SetPointer(0, nullptr, FILE_BEGIN);

	SetFileSize();

	if (!m_DisplayMode.touched())
	{
		m_DumpTextMode = vo.DetectDumpMode && isBinaryFile(m_Codepage);
		m_DisplayMode = m_DumpTextMode ? VMT_DUMP : VMT_TEXT;
		m_DisplayMode.forget();
	}

	if (FilePos > FileSize)
		FilePos=0;
	if ( FilePos )
		AdjustFilePos();

	ChangeViewKeyBar();
	AdjustWidth();

	const auto update_check_period = [&]
	{
		// media inserted here
		switch (FAR_GetDriveType(GetPathRoot(strFullFileName), 2)) //??? make it configurable
		{
			case DRIVE_REMOVABLE: return 0ms;
			case DRIVE_USBDRIVE:  return 500ms;
			case DRIVE_FIXED:     return 1ms;
			case DRIVE_REMOTE:    return 500ms;
			case DRIVE_CDROM:     return 0ms;
			case DRIVE_RAMDISK:   return 1ms;
			default:              return 0ms;
		}
	}();

	if (update_check_period != 0s)
	{
		m_TimeCheck = std::make_unique<time_check>(time_check::mode::delayed, update_check_period);
	}

	if (!HostFileViewer) ReadEvent();

	return true;
}

bool Viewer::isBinaryFile(uintptr_t cp) // very approximate: looks for '\0' in first 2k bytes
{
	char Buffer[2048];
	const auto CurrentPos = vtell();
	vseek(0, FILE_BEGIN);
	size_t BytesRead = 0;
	bool Result = ViewFile.Read(Buffer, sizeof(Buffer), BytesRead);
	vseek(CurrentPos, FILE_BEGIN);

	if (!Result)
		return true;

	if (IsUnicodeCodePage(cp))
	{
		return contains(span(reinterpret_cast<const wchar_t*>(Buffer), BytesRead / sizeof(wchar_t)), L'\0');
	}
	else
	{
		return contains(span(Buffer, BytesRead), '\0');
	}
}

void Viewer::AdjustWidth()
{
	Width = m_Where.width();
	XX2 = m_Where.right;

	if (ViOpt.ShowScrollbar && !m_bQuickView)
	{
		Width--;
		XX2--;
	}
}

bool Viewer::CheckChanged()
{
	os::fs::find_data NewViewFindData;
	if (!os::fs::get_find_data(strFullFileName, NewViewFindData))
		return true;

	// Smart file change check -- thanks Dzirt2005
	//
	bool changed = ViewFindData.LastWriteTime != NewViewFindData.LastWriteTime || ViewFindData.FileSize != NewViewFindData.FileSize;
	if (changed)
	{
		ViewFindData = NewViewFindData;
	}
	else
	{
		if (!ViewFile.GetSize(NewViewFindData.FileSize) || FileSize == static_cast<long long>(NewViewFindData.FileSize))
			return true;

		changed = FileSize > static_cast<long long>(NewViewFindData.FileSize); // true if file shrank
	}

	SetFileSize();
	if (changed) // do not reset caches if file just enlarged [make sense on Win7, doesn't matter on XP]
	{
		Reader.Clear(); // иначе зачем вся эта возня?
		ViewFile.FlushBuffers();
		vseek(0, FILE_CURRENT); // reset vgetc state
		lcache_ready = false; // reset start-lines cache
	}

	return changed;
}

void Viewer::ShowPage(int nMode)
{
	redraw_selection = false;

	AdjustWidth();

	if (!ViewFile)
	{
		if (!strFileName.empty() && ((nMode == SHOW_RELOAD) || (nMode == SHOW_HEX)|| (nMode == SHOW_DUMP)))
		{
			SetScreen(m_Where, L' ', colors::PaletteColorToFarColor(COL_VIEWERTEXT));
			GotoXY(m_Where.left, m_Where.top);
			SetColor(COL_WARNDIALOGTEXT);
			Text(cut_right(msg(lng::MViewerCannotOpenFile), XX2 - m_Where.left + 1));
			ShowStatus();
		}

		return;
	}

	if (m_HideCursor)
		SetCursorType(false, 10);

	vseek(FilePos, FILE_BEGIN);
	LastPage = false;

	if ( SelectSize < 0 )
		SelectPos=FilePos;

	switch (nMode)
	{
		case SHOW_HEX:
			ShowHex();
			break;
		case SHOW_DUMP:
			ShowDump();
			break;
		case SHOW_RELOAD:
			{
				if (m_TimeCheck)
					m_TimeCheck->reset();

				CheckChanged();

				Strings.clear();

				for (int Y = m_Where.top; Y <= m_Where.bottom; ++Y)
				{
					ViewerString NewString;
					NewString.nFilePos = vtell();

					if (Y == m_Where.top + 1 && !veof())
						SecondPos=vtell();

					ReadString(&NewString, -1);
					Strings.emplace_back(NewString);
				}
			}
			break;

		case SHOW_UP:
			{
				ViewerString NewString;
				NewString.nFilePos = FilePos;

				SecondPos = Strings.front().nFilePos;

				Strings.emplace_front(NewString);
				Strings.pop_back();

				ReadString(&Strings.front(), (int)(SecondPos - FilePos));
			}
			break;

		case SHOW_DOWN:
			{
				ViewerString NewString;
				NewString.nFilePos = Strings.back().nFilePos + Strings.back().linesize;

				Strings.emplace_back(NewString);
				Strings.pop_front();

				FilePos = Strings.front().nFilePos;
				SecondPos = FilePos + Strings.front().linesize;

				vseek(Strings.back().nFilePos, FILE_BEGIN);
				ReadString(&Strings.back(), -1);
			}
			break;
	}

	if (nMode != SHOW_HEX && nMode != SHOW_DUMP)
	{
		int Y = m_Where.top - 1;
		for (auto& i: Strings)
		{
			++Y;
			SetColor(COL_VIEWERTEXT);
			GotoXY(m_Where.left, Y);

			if (static_cast<long long>(i.Data.size()) > LeftPos)
			{
				Text(fit_to_left(i.Data.substr(LeftPos), Width));
			}
			else
			{
				Text(string(Width, L' '));
			}

			if (SelectSize >= 0 && i.bSelection)
			{
				long long SelX1;

				if (LeftPos > i.nSelStart)
					SelX1 = m_Where.left;
				else
					SelX1 = i.nSelStart - LeftPos;

				if (!m_Wrap && (i.nSelEnd < LeftPos || i.nSelStart > LeftPos + XX2 - m_Where.left))
				{
					if (AdjustSelPosition)
					{
						LeftPos = std::max(i.nSelStart - 1, 0LL);
						AdjustSelPosition = false;
						Show();
						return;
					}
				}
				else
				{
					SetColor(COL_VIEWERSELECTEDTEXT);
					GotoXY(static_cast<int>(m_Where.left + SelX1), Y);
					long long Length = i.nSelEnd - i.nSelStart;

					if (LeftPos > i.nSelStart)
						Length = i.nSelEnd - LeftPos;

					if (LeftPos > i.nSelEnd)
						Length = 0;

					Text(cut_right(i.Data.substr(SelX1 + LeftPos), Length));
				}
			}

			if (static_cast<long long>(i.Data.size()) > LeftPos + Width && ViOpt.ShowArrows)
			{
				GotoXY(XX2,Y);
				SetColor(COL_VIEWERARROWS);
				BoxText(0xbb);
			}

			if (LeftPos>0 && !i.Data.empty() && ViOpt.ShowArrows)
			{
				GotoXY(m_Where.left, Y);
				SetColor(COL_VIEWERARROWS);
				BoxText(0xab);
			}
		}
	}

	DrawScrollbar();
	ShowStatus();
}

void Viewer::DisplayObject()
{
	SHOW_MODES ShowMode;

	switch (m_DisplayMode)
	{
	default:
	case VMT_TEXT:
		ShowMode = SHOW_RELOAD;
		break;

	case VMT_HEX:
		ShowMode = SHOW_HEX;
		break;

	case VMT_DUMP:
		ShowMode = SHOW_DUMP;
		break;
	}

	ShowPage(ShowMode);
}


int Viewer::getCharSize() const
{
	if (CP_UTF8 == m_Codepage)
		return -1;
	else if (CP_UNICODE == m_Codepage || CP_REVERSEBOM == m_Codepage)
		return +2;
	else
		return m_Codepage == MB.GetCP()? -static_cast<int>(MB.GetSize()) : +1;
}

static int getChSize( UINT cp )
{
	if ( CP_UNICODE == cp || CP_REVERSEBOM == cp )
		return +2;
	else
		return +1;
}

int Viewer::GetModeDependentCharSize() const
{
	return m_DisplayMode == VMT_HEX? 1 : getChSize(m_Codepage);
}

int Viewer::GetModeDependentLineSize() const
{
	return m_DisplayMode == VMT_HEX? 16 : Width * getChSize(m_Codepage);
}

int Viewer::txt_dump(std::string_view const Str, size_t ClientWidth, string& OutStr, wchar_t ZeroChar, int tail) const
{
	OutStr.clear();

	if (m_Codepage == CP_UNICODE || m_Codepage == CP_REVERSEBOM)
	{
		OutStr.assign(reinterpret_cast<const wchar_t*>(Str.data()), Str.size() / sizeof(wchar_t));
		if (m_Codepage == CP_REVERSEBOM)
		{
			swap_bytes(OutStr.data(), OutStr.data(), OutStr.size() * sizeof(wchar_t));
		}
		if (Str.size() & 1)
		{
			OutStr.push_back(Utf::REPLACE_CHAR);
		}
	}
	else if (m_Codepage == CP_UTF8)
	{
		std::vector<wchar_t> Buffer(ClientWidth);
		int dummy_tail;
		const auto WideCharsNumber = Utf8::get_chars(Str, Buffer.data(), ClientWidth, dummy_tail);
		for (size_t iw = 0; OutStr.size() < ClientWidth && iw != WideCharsNumber; ++iw)
		{
			if (tail)
			{
				--tail;
				OutStr.push_back(Utf::CONTINUE_CHAR);
				continue;
			}

			OutStr.push_back(Buffer[iw] == Utf::BOM_CHAR? Utf::REPLACE_CHAR : Buffer[iw]); // BOM can be Zero Length
			const auto clen = encoding::utf8::get_bytes_count({ Buffer.data() + iw, 1 });
			const auto PaddingSize = std::min(clen - 1, static_cast<size_t>(ClientWidth) - OutStr.size());
			OutStr.append(PaddingSize, Utf::CONTINUE_CHAR);
			tail = static_cast<int>(clen - 1 - PaddingSize); // char continues on the next line?
		}
	}
	else if (m_Codepage == MB.GetCP())
	{
		while (OutStr.size() < ClientWidth && OutStr.size() < Str.size())
		{
			if (tail)
			{
				--tail;
				OutStr.push_back(Utf::CONTINUE_CHAR);
				continue;
			}
			wchar_t Char;
			const auto clen = MB.GetChar(Str.substr(OutStr.size()), Char);
			if (clen)
			{
				OutStr.push_back(Char);
			}
			else
			{
				OutStr.push_back(Utf::REPLACE_CHAR);
				continue;
			}

			const auto PaddingSize = std::min(clen - 1, ClientWidth - OutStr.size());
			OutStr.append(PaddingSize, Utf::CONTINUE_CHAR);

			tail = static_cast<int>(clen - 1 - PaddingSize); // char continues on the next line?
		}
	}
	else
	{
		OutStr = encoding::get_chars(m_Codepage, Str);
	}

	OutStr.resize(ClientWidth, L' ');
	std::replace(ALL_RANGE(OutStr), L'\0', ZeroChar);

	return tail;
}


void Viewer::ShowDump()
{
	const int CharSize = getChSize(m_Codepage);
	const int xl = m_Codepage == CP_UTF8? 4 - 1 : m_Codepage == MB.GetCP()? static_cast<int>(MB.GetSize() - 1) : 0;
	const size_t BytesToRead = Width * CharSize + xl;
	const DWORD mb = Width * CharSize;

	FilePos -= FilePos % CharSize;
	vseek(SecondPos = FilePos, FILE_BEGIN);

	bool EndFile = false;
	int tail = 0;
	std::vector<char> line(Width * 2);
	string OutStr;

	for (auto Y = m_Where.top; Y <= m_Where.bottom; ++Y)
	{
		SetColor(COL_VIEWERTEXT);
		GotoXY(m_Where.left, Y);

		if (EndFile)
		{
			Text(string(ObjWidth(), L' '));
			continue;
		}
		const auto bpos = vtell();
		if (Y == m_Where.top + 1)
			SecondPos = bpos;

		size_t BytesRead = 0;
		Reader.Read(line.data(), BytesToRead, &BytesRead);
		if (BytesRead > mb)
			Reader.Unread(BytesRead-mb);
		else
			LastPage = EndFile = veof();

		tail = txt_dump({ line.data(), BytesRead }, Width, OutStr, ZeroChar(), tail);

		Text(fit_to_left(OutStr, ObjWidth()));
		if ( SelectSize > 0 && bpos < SelectPos+SelectSize && bpos+mb > SelectPos )
		{
			const int bsel = SelectPos > bpos? (int)(SelectPos-bpos) / CharSize : 0;
			const int esel = SelectPos + SelectSize < bpos + mb? ((int)(SelectPos + SelectSize - bpos) + CharSize - 1) / CharSize : Width;
			SetColor(COL_VIEWERSELECTEDTEXT);
			GotoXY(bsel, Y);
			Text(cut_right(OutStr.substr(bsel), esel - bsel));
		}
	}
}

void Viewer::ShowHex()
{
	const auto HexLeftPos = ((LeftPos > 80 - ObjWidth())? std::max(80 - ObjWidth(), 0) : LeftPos);
	const string BorderLine{ BoxSymbols[BS_V1], L' '};

	int tail = 0;
	bool EndFile = false;

	LastPage = false;

	for (auto Y = m_Where.top; Y <= m_Where.bottom; ++Y)
	{
		bool bSelStartFound = false;
		bool bSelEndFound = false;

		SetColor(COL_VIEWERTEXT);
		GotoXY(m_Where.left, Y);

		if (EndFile)
		{
			Text(string(ObjWidth(), L' '));
			continue;
		}

		if (Y == m_Where.top + 1 && !veof())
			SecondPos=vtell();

		auto OutStr = format(L"{0:010X}: "sv, vtell());
		int SelStart = static_cast<int>(OutStr.size()), SelEnd = SelStart;
		long long fpos = vtell();

		if (fpos > SelectPos)
			bSelStartFound = true;

		if (fpos < SelectPos+SelectSize-1)
			bSelEndFound = true;

		if ( SelectSize < 0 )
			bSelStartFound = bSelEndFound = false;

		char RawBuffer[16+3];
		size_t BytesRead = 0;
		size_t BytesToRead = CP_UTF8 == m_Codepage ? 16 + 4 - 1 : (m_Codepage == MB.GetCP()? 16 + MB.GetSize() - 1 : 16);
		Reader.Read(RawBuffer, BytesToRead, &BytesRead);
		if (BytesRead > 16)
			Reader.Unread(BytesRead-16);
		else
			LastPage = EndFile = veof();

		string TextStr;
		if (!BytesRead)
		{
			OutStr.clear();
		}
		else
		{
			if (IsUnicodeCodePage(m_Codepage))
			{
				const int be = m_Codepage == CP_REVERSEBOM ? 1 : 0;
				for (size_t X = 0; X != 16; X += 2)
				{
					if (SelectSize >= 0 && (SelectPos == fpos || SelectPos == fpos+1))
					{
						const auto half = SelectPos != fpos;
						bSelStartFound = true;
						SelStart = static_cast<int>(OutStr.size()) + (half ? 1 + be : 0);
						if (!SelectSize)
							SelStart += (half ? be : 0) - 1;
					}
					if (SelectSize >= 0 && (fpos == SelectPos+SelectSize-1 || fpos+1 == SelectPos+SelectSize-1))
					{
						const auto half = fpos == SelectPos + SelectSize - 1;
						bSelEndFound = true;
						SelEnd = static_cast<int>(OutStr.size()) + 3 - (half ? 1 + be : 0);
						if (!SelectSize)
							SelEnd = SelStart;
					}
					else if ( SelectSize == 0 && (SelectPos == fpos || SelectPos == fpos+1) )
					{
						bSelEndFound = true;
						SelEnd = SelStart;
					}

					if (X < BytesRead - 1) // full character
					{
						auto Char = *reinterpret_cast<wchar_t*>(RawBuffer + X);
						if (be)
						{
							swap_bytes(&Char, &Char, sizeof(wchar_t));
						}
						OutStr += format(L"{0:04X} "sv, int(Char));
						TextStr.push_back(Char? Char : ZeroChar());
					}
					else if (X == BytesRead - 1) // half character only
					{
						const auto GoodHalf = format(L"{0:02X}"sv, int(RawBuffer[X]));
						const auto BadHalf = L"xx"s;
						OutStr += (be? GoodHalf : BadHalf) + (be? BadHalf : GoodHalf);
						OutStr.push_back(L' ');
						TextStr.push_back(Utf::REPLACE_CHAR);
					}
					else // no character
					{
						OutStr.append(5, L' ');
						TextStr.push_back(L' ');
					}

					if (X == 3*2)
					{
						OutStr += BorderLine;
					}
					fpos += 2;
				}
			}
			else
			{
				if ( SelectSize >= 0 )
				{
					if (SelectPos >= fpos && SelectPos < fpos+16)
					{
						const auto off = static_cast<int>(SelectPos - fpos);
						bSelStartFound = true;
						SelStart = static_cast<int>(OutStr.size() + 3 * off + (off < 8? 0 : BorderLine.size()));
						if (!SelectSize)
							--SelStart;
					}
					const auto selectEnd = SelectPos + SelectSize - 1;
					if (selectEnd >= fpos && selectEnd < fpos+16)
					{
						const auto off = static_cast<int>(selectEnd - fpos);
						bSelEndFound = true;
						SelEnd = SelectSize? static_cast<int>(OutStr.size() + 3 * off + (off < 8? 0 : BorderLine.size()) + 1) : SelStart;
					}
					else if ( SelectSize == 0 && SelectPos == fpos )
					{
						bSelEndFound = true;
						SelEnd = SelStart;
					}
				}

				for (size_t X = 0; X != 16; ++X)
				{
					if (X < BytesRead)
						OutStr += format(L"{0:02X} "sv, int(RawBuffer[X]));
					else
						OutStr.append(3, L' ');

					if (X == 7)
						OutStr += BorderLine;
				}
				tail = txt_dump({ RawBuffer, BytesRead }, 16, TextStr, ZeroChar(), tail);
			}
		}

		if ((SelEnd <= SelStart) && bSelStartFound && bSelEndFound && SelectSize > 0 )
			SelEnd = static_cast<int>(OutStr.size()) - 2;

		OutStr.push_back(L' ');
		OutStr += TextStr;

		if (static_cast<int>(OutStr.size()) > HexLeftPos)
		{
			Text(fit_to_left(OutStr.substr(HexLeftPos), ObjWidth()));
		}
		else
		{
			Text(string(ObjWidth(), L' '));
		}

		if (bSelStartFound && bSelEndFound)
		{
			SetColor(COL_VIEWERSELECTEDTEXT);
			GotoXY(static_cast<int>(static_cast<long long>(m_Where.left) + SelStart - HexLeftPos), Y);
			Text(cut_right(OutStr.substr(SelStart), SelEnd - SelStart + 1));
		}
	}
}

/* $ 27.04.2001 DJ
   отрисовка скроллбара - в отдельную функцию
*/
void Viewer::DrawScrollbar()
{
	if (ViOpt.ShowScrollbar)
	{
		SetColor(m_bQuickView? COL_PANELSCROLLBAR : COL_VIEWERSCROLLBAR);

		UINT x = m_Where.right + (m_bQuickView? 1 : 0);
		UINT h = m_Where.height();
		unsigned long long start, end, total;

		if (m_DisplayMode == VMT_TEXT)
		{
			total = static_cast<unsigned long long>(FileSize);
			start = static_cast<unsigned long long>(FilePos);
			ViewerString& last_line = Strings.back();
			end = last_line.nFilePos + last_line.linesize;
			if ( end == static_cast<unsigned long long>(FileSize) && last_line.linesize > 0 && last_line.eol_length != 0 )
				++total;
		}
		else
		{
			const auto LineSize = GetModeDependentLineSize();
			total = FileSize / LineSize + ((FileSize % LineSize)? 1 : 0);
			start = FilePos / LineSize + ((FilePos % LineSize)? 1 : 0);
			end = start + h;
		}
		ScrollBarEx3(x, m_Where.top, h, start, end, total);
	}
}


string Viewer::GetTitle() const
{
	return strTitle.empty()? strFullFileName : strTitle;
}

void Viewer::ShowStatus() const
{
	if (HostFileViewer)
		HostFileViewer->ShowStatus();
}


void Viewer::SetStatusMode(int Mode)
{
	ShowStatusLine=Mode;
}


static bool is_word_div(const wchar_t ch)
{
	static const wchar_t extra_div[] = { Utf::BOM_CHAR, Utf::REPLACE_CHAR };
	return !ch || std::iswspace(ch) || contains(Global->Opt->strWordDiv.Get(), ch) || contains(extra_div, ch);
}

static bool wrapped_char(const wchar_t ch)
{
	return IsBlankOrEos(ch) || contains(L",;>)"sv, ch);
}

void Viewer::ReadString(ViewerString *pString, int MaxSize, bool update_cache)
{
	AdjustWidth();

	int OutPtr = 0, nTab = 0, wrap_out = -1;
	wchar_t ch, eol_char = L'\0';
	long long wrap_pos = -1;
	bool skip_space = false;

	if (m_DisplayMode != VMT_TEXT)
	{
		vseek(GetModeDependentLineSize(), FILE_CURRENT);
		ReadBuffer[OutPtr] = L'\0';
		LastPage = veof();
		return;
	}

	bool bSelStartFound = false, bSelEndFound = false;
	pString->bSelection = false;
	long long sel_end = SelectPos + SelectSize;

	long long fpos1 = vtell();
	for (;;)
	{
		long long fpos = fpos1;

		if (OutPtr >= static_cast<int>(MaxViewLineSize()))
			break;

		if (--nTab >= 0)
			ch = L' ';
		else
		{
			if (!MaxSize-- || !vgetc(&ch))
				break;
		}
		fpos1 = vtell();

		if (SelectSize >= 0)
		{
			if (fpos == SelectPos || (fpos < SelectPos && fpos1 > SelectPos))
			{
				pString->nSelStart = OutPtr;
				bSelStartFound = true;
			}
			if (fpos == sel_end || (fpos < sel_end && fpos1 > sel_end))
			{
				pString->nSelEnd = OutPtr + (fpos < sel_end ? 1 : 0);
				bSelEndFound = true;
			}
		}

		if (!fpos && Utf::BOM_CHAR == ch)
		{
			continue; // skip BOM
		}
		else if (L'\t' == ch)
		{
			nTab = ViOpt.TabSize - (OutPtr % ViOpt.TabSize);
			continue;
		}
		if (IsEol(ch))
		{
			eol_char = ch;
			break;
		}

		ReadBuffer[OutPtr++] = ch? ch : ZeroChar();
		if ( !m_Wrap )
			continue;

		if ( m_WordWrap && OutPtr <= Width && wrapped_char(ch))
		{
			wrap_out = OutPtr;
			wrap_pos = fpos1;
		}

		if ( OutPtr < Width )
			continue;
		if ( !m_WordWrap )
			break;

		if ( OutPtr > Width )
		{
			if ( wrap_out <= 0 || IsBlankOrEos(ch) )
			{
				wrap_out = OutPtr - 1;
				wrap_pos = fpos;
			}

			OutPtr = wrap_out;
			vseek(wrap_pos, FILE_BEGIN);
			while (OutPtr > 0 && IsBlankOrEos(ReadBuffer[OutPtr-1]))
				--OutPtr;

			if ( bSelEndFound && pString->nSelEnd > OutPtr )
				pString->nSelEnd = OutPtr;
			if ( bSelStartFound && pString->nSelStart >= OutPtr )
				bSelStartFound = bSelEndFound = false;

			skip_space = true;
			break;
		}
	}

	int eol_len = (eol_char ? 1 : 0);
	if (skip_space || eol_char != L'\n') // skip spaces and/or eol-s if required
	{
		for (;;)
		{
			vgetc(nullptr);
			int ib = vgetc_ib;
			if (!vgetc(&ch))
				break;

			if (skip_space && !eol_char && IsBlankOrEos(ch))
				continue;

			if ( ch == L'\n' )
			{
				++eol_len;            // LF or CRLF
				assert(eol_len <= 2);
			}
			else if ( ch != L'\r' )	 // nor LF nor CR
			{
				vgetc_ib = ib;        // ungetc(1)
				assert(eol_len <= 1); // CR or unterminated
			}
			else                     // CR
			{
				eol_char = ch;
				if (++eol_len == 1)	 // single CR - continue
					continue;

				assert(eol_len == 2); // CRCR...
				if (vgetc(&ch) && ch == L'\n')
					++eol_len;         // CRCRLF
				else
				{
					assert(ib + 2 <= vgetc_ib);
					vgetc_ib = ib;     // CR ungetc(2)
					eol_len = 1;
				}
			}
			break;
		}
	}

	pString->eol_length = eol_len;
	ReadBuffer[OutPtr]=0;
	pString->linesize = (int)(vtell() - pString->nFilePos);

	if ( update_cache )
		CacheLine(pString->nFilePos, pString->linesize, pString->eol_length != 0);

	if (SelectSize >= 0 && OutPtr > 0)
	{
		if (!bSelStartFound && pString->nFilePos >= SelectPos && pString->nFilePos <= sel_end)
		{
			bSelStartFound = true;
			pString->nSelStart = 0;
		}
		if (bSelStartFound && !bSelEndFound)
		{
			bSelEndFound = true;
			pString->nSelEnd = OutPtr;
		}
		if (bSelEndFound && pString->nSelEnd > OutPtr)
			pString->nSelEnd = OutPtr;

		pString->bSelection = bSelStartFound && bSelEndFound;
	}

	if (!eol_char && veof())
		LastPage = true;

	pString->Data = ReadBuffer.data();
}


long long Viewer::EndOfScreen(int line)
{
	long long pos;

	if (m_DisplayMode == VMT_TEXT)
	{
		const auto i = std::next(Strings.begin(), m_Where.height() - 1 + line);
		pos = i->nFilePos + i->linesize;
		if (!line && !m_Wrap && Strings.back().linesize > 0)
		{
			vseek(Strings.back().nFilePos, FILE_BEGIN);
			int col = 0, rmargin = (int)LeftPos + Width;
			wchar_t ch;
			for (;;)
			{
				if ( !vgetc(&ch) )
					break;
				if (IsEol(ch))
					break;
				if ( ch == L'\t' )
					col += ViOpt.TabSize - (col % ViOpt.TabSize);
				else
					++col;
				if ( col >= rmargin )
				{
					pos = vtell();
					break;
				}
			}
		}
	}
	else
	{
		pos = FilePos + GetModeDependentLineSize() * (m_Where.height() + line);
	}
	if (pos < 0)
		pos = 0;
	else if (pos > FileSize)
		pos = FileSize;

	return pos;
}

long long Viewer::BegOfScreen()
{
	long long pos = FilePos;

	if (m_DisplayMode == VMT_TEXT && !m_Wrap && LeftPos > 0)
	{
		vseek(FilePos, FILE_BEGIN);
		int col = 0;
		wchar_t ch;
		pos = -1;
		long long prev_pos;
		for (;;)
		{
			prev_pos = vtell();
			if ( !vgetc(&ch) )
				break;
			if (IsEol(ch))
			{
				pos = std::next(Strings.begin())->nFilePos;
				break;
			}
			if ( ch == L'\t' )
				col += ViOpt.TabSize - (col % ViOpt.TabSize);
			else
				++col;
			if ( col > LeftPos )	//!! шеврон закрывает первый символ
				break;				//!! при LeftPos=1 не видны 2 символа
		}
		if ( pos < 0 )
			pos = (col > LeftPos ? prev_pos : vtell());
	}

	return pos;
}

long long Viewer::XYfilepos(int col, int row)
{
	long long pos = -1;

	int csz = getChSize(m_Codepage);
	switch (m_DisplayMode)
	{
	case VMT_DUMP:
		pos = FilePos + csz*(Width*row + col);
		break;

	case VMT_HEX:
		if (csz < 2)
		{
		//0000000000: 32 30 2E 30 31 2E 32 30 | 31 35 20 31 30 3A 33 39  20.01.2015 10:39
			if      (col < 11) col = 0;
			else if (col < 35) col = (col-11)/3;
			else if (col < 37) col = 8;
			else if (col < 61) col = 8 + (col-37)/3;
			else if (col < 63) col = 0;
			else if (col < 79) col = col-63;
			else               col = 16;
		}
		else
		{
		//0000000020: 0031 002E 0030 0022 | 0020 0065 006E 0063  1.0" enc
			if      (col < 11) col = 0;
			else if (col < 31) col = (col-11)/5;
			else if (col < 33) col = 4;
			else if (col < 53) col = 4 + (col-33)/5;
			else if (col < 55) col = 0;
			else if (col < 63) col = col-55;
			else               col = 8;
		}
		pos = FilePos + 16*row + csz*col;
		break;

	case VMT_TEXT:
		for (auto& i: Strings)
		{
			if (i.linesize <= 0)
			{
				pos = i.nFilePos;
				break;
			}

			if (--row < 0)
			{
				vseek(i.nFilePos, FILE_BEGIN);
				if (!m_Wrap) col += (int)LeftPos;
				int clm = 0;
				wchar_t ch;
				for (;;)
				{
					pos = vtell();
					if (clm >= col)
						break;

					if (!vgetc(&ch))
					{
						pos = i.nFilePos + i.linesize;
						break;
					}
					else if (IsEol(ch))
					{
						pos = i.nFilePos + i.linesize - 1;
						break;
					}
					else if (ch == L'\t')
					{
						clm += ViOpt.TabSize - (clm % ViOpt.TabSize);
						if (clm > col)
							break;
					}
					else
						++clm;
				}
				break;
			}
		}
		break;

	default:
		return -1;
	}
	return std::clamp(pos, 0ll, FileSize);
}


long long Viewer::VMProcess(int OpCode,void *vParam,long long iParam)
{
	switch (OpCode)
	{
		case MCODE_C_EMPTY:
			return !FileSize;
		case MCODE_C_SELECTED:
			return SelectSize >= 0;
		case MCODE_C_EOF:
			return LastPage || !ViewFile;
		case MCODE_C_BOF:
			return !FilePos || !ViewFile;
		case MCODE_V_VIEWERSTATE:
		{
			DWORD MacroViewerState = 0;
			MacroViewerState |= ViOpt.AutoDetectCodePage?                             0_bit : 0;
			MacroViewerState |= IsUnicodeCodePage(m_Codepage)?                        2_bit : 0;
			MacroViewerState |= m_Wrap?                                               3_bit : 0;
			MacroViewerState |= m_WordWrap?                                           4_bit : 0;
			MacroViewerState |= m_DisplayMode == VMT_HEX?                             5_bit : 0;
			MacroViewerState |= m_DisplayMode == VMT_DUMP?                            6_bit : 0;
			MacroViewerState |= HostFileViewer && !HostFileViewer->GetCanLoseFocus()? 11_bit : 0;
			MacroViewerState |= Global->OnlyEditorViewerUsed?                         27_bit | 11_bit : 0;
			return MacroViewerState;
		}
		case MCODE_V_ITEMCOUNT: // ItemCount - число элементов в текущем объекте
			return GetViewFileSize();
		case MCODE_V_CURPOS: // CurPos - текущий индекс в текущем объекте
			return GetViewFilePos()+1;
	}

	return 0;
}

bool Viewer::ProcessKey(const Manager::Key& Key)
{
	const auto ret = process_key(Key);
	if (redraw_selection)
		Show();
	return ret;
}

bool Viewer::process_key(const Manager::Key& Key)
{
	unsigned int LocalKey = Key();

	if ((LocalKey & ~KEY_SHIFT) == 0)
		LocalKey = KEY_NONE;

	if (LocalKey != KEY_NONE)
		m_IdleCheck->reset();
	else
	{
		if (*m_IdleCheck)
			LocalKey = KEY_IDLE;
		else
			os::chrono::sleep_for(10ms);
	}

	if (!ViOpt.PersistentBlocks &&
		LocalKey!=KEY_IDLE && LocalKey!=KEY_NONE && !(LocalKey==KEY_CTRLINS||LocalKey==KEY_RCTRLINS||LocalKey==KEY_CTRLNUMPAD0||LocalKey==KEY_RCTRLNUMPAD0) &&
		LocalKey!=KEY_CTRLC && LocalKey!=KEY_RCTRLC &&
		LocalKey!=KEY_SHIFTF7 && LocalKey!=KEY_SPACE && LocalKey!=KEY_ALTF7 && LocalKey!=KEY_RALTF7 )
	{
		redraw_selection = SelectSize >= 0;
		SelectSize = -1;
	}

	if (!InternalKey && !LastKeyUndo && (UndoData.empty() || FilePos!=UndoData.back().UndoAddr || LeftPos!=UndoData.back().UndoLeft))
	{
		enum { VIEWER_UNDO_COUNT = 65536 };

		if (UndoData.size() == VIEWER_UNDO_COUNT)
			UndoData.pop_front();
		UndoData.emplace_back(FilePos, LeftPos);
	}

	if (LocalKey!=KEY_ALTBS && LocalKey!=KEY_RALTBS && LocalKey!=KEY_CTRLZ && LocalKey!=KEY_RCTRLZ && LocalKey!=KEY_NONE && LocalKey!=KEY_IDLE)
		LastKeyUndo=FALSE;

	if (LocalKey>=KEY_CTRL0 && LocalKey<=KEY_CTRL9)
	{
		int Pos=LocalKey-KEY_CTRL0;

		if (BMSavePos[Pos].FilePos != POS_NONE)
		{
			FilePos = BMSavePos[Pos].FilePos;
			LeftPos = BMSavePos[Pos].LeftPos;
			Show();
		}

		return true;
	}

	if (LocalKey>=KEY_CTRLSHIFT0 && LocalKey<=KEY_CTRLSHIFT9)
		LocalKey=LocalKey-KEY_CTRLSHIFT0+KEY_RCTRL0;
	else if (LocalKey>=KEY_RCTRLSHIFT0 && LocalKey<=KEY_RCTRLSHIFT9)
		LocalKey&=~KEY_SHIFT;

	if (LocalKey>=KEY_RCTRL0 && LocalKey<=KEY_RCTRL9)
	{
		int Pos=LocalKey-KEY_RCTRL0;
		BMSavePos[Pos].FilePos = FilePos;
		BMSavePos[Pos].LeftPos = LeftPos;
		return true;
	}

	switch (LocalKey)
	{
		case KEY_F1:
			help::show(L"Viewer"sv);
			return true;

		case KEY_CTRLU:
		case KEY_RCTRLU:
		{
			SelectSize = -1;
			Show();
			return true;
		}
		case KEY_CTRLC:
		case KEY_RCTRLC:
		case KEY_CTRLINS:  case KEY_CTRLNUMPAD0:
		case KEY_RCTRLINS: case KEY_RCTRLNUMPAD0:
		{
			if (SelectSize >= 0 && ViewFile)
			{
				wchar_t_ptr_n<256> SelData(SelectSize);
				long long CurFilePos=vtell();
				vseek(SelectPos, FILE_BEGIN);
				vread(SelData.get(), (int)SelectSize);
				SetClipboardText({ SelData.get(), static_cast<size_t>(SelectSize) });
				vseek(CurFilePos, FILE_BEGIN);
			}
			return true;
		}
		//   включить/выключить скролбар
		case KEY_CTRLS:
		case KEY_RCTRLS:
		{
			ViOpt.ShowScrollbar=!ViOpt.ShowScrollbar;
			Global->Opt->ViOpt.ShowScrollbar=ViOpt.ShowScrollbar;

			if (m_bQuickView)
				Global->CtrlObject->Cp()->ActivePanel()->Redraw();

			Show();
			return true;
		}
		case KEY_IDLE:
		{
			if (Global->Opt->ViewerEditorClock && HostFileViewer && HostFileViewer->IsFullScreen() && Global->Opt->ViOpt.ShowTitleBar)
				ShowTime();

			if (ViewFile)
			{
				if (m_TimeCheck && !*m_TimeCheck)
					return true;

				CheckChanged();

				if (FilePos > FileSize)
				{
					ProcessKey(Manager::Key(KEY_CTRLEND));
				}
				else
				{
					const auto PrevLastPage = LastPage;
					LastPage = false;
					Show();

					if (PrevLastPage && !LastPage)
					{
						ProcessKey(Manager::Key(KEY_CTRLEND));
						LastPage = true;
					}
				}
			}
			return true;
		}
		case KEY_ALTBS:
		case KEY_RALTBS:
		case KEY_CTRLZ:
		case KEY_RCTRLZ:
		{
			if (!UndoData.empty())
			{
				UndoData.pop_back();
				if (!UndoData.empty())
				{
					FilePos=UndoData.back().UndoAddr;
					LeftPos=UndoData.back().UndoLeft;
					Show();
				}
			}
			return true;
		}
		case KEY_ADD:
		case KEY_SUBTRACT:
		{
			if (!ViewNamesList.empty())
			{
				string strName;
				if (LocalKey == KEY_ADD? ViewNamesList.GetNextName(strName) : ViewNamesList.GetPrevName(strName))
				{
					SavePosition();
					BMSavePos.Clear(); //Prepare for new file loading

					if (OpenFile(strName, TRUE))
					{
						SecondPos=0;
						Show();
						if (HostFileViewer) HostFileViewer->OnReload();
					}

					ShowConsoleTitle();
				}
			}

			return true;
		}
		case KEY_SHIFTF2:
		{
			ProcessTypeWrapMode(!m_WordWrap);
			return true;
		}
		case KEY_F2:
		{
			if (m_DisplayMode == VMT_TEXT)
				ProcessWrapMode(!m_Wrap);
			else {
				m_DisplayMode = m_DisplayMode == VMT_DUMP || m_DumpTextMode ? VMT_TEXT : VMT_DUMP;
				ProcessDisplayMode(m_DisplayMode);
			}
			return true;
		}
		case KEY_F4:
		{
			m_DisplayMode = m_DisplayMode == VMT_HEX? (m_DumpTextMode? VMT_DUMP : VMT_TEXT) : VMT_HEX;
			ProcessDisplayMode(m_DisplayMode);
			return true;
		}
		case KEY_SHIFTF4:
		{
			const menu_item ModeListMenu[]
			{
				{ msg(lng::MViewF4Text), 0 },
				{ msg(lng::MViewF4), 0 },
				{ msg(lng::MViewF4Dump), 0},
			};
			int MenuResult;
			{
				const auto vModes = VMenu2::create(msg(lng::MViewMode), ModeListMenu, ScrY - 4);
				vModes->SetMenuFlags(VMENU_WRAPMODE | VMENU_AUTOHIGHLIGHT);
				vModes->SetSelectPos(m_DisplayMode, +1);
				MenuResult = vModes->Run();
			}
			if (MenuResult >= 0)
			{
				const auto NewMode = static_cast<VIEWER_MODE_TYPE>(MenuResult);
				if (NewMode != m_DisplayMode)
				{
					if (NewMode != VMT_HEX)
						m_DumpTextMode = NewMode == VMT_DUMP;
					ProcessDisplayMode(NewMode);
				}
			}
			return true;
		}
		case KEY_F7:
		{
			Search(0,nullptr);
			return true;
		}
		case KEY_SHIFTF7:
		case KEY_SPACE:
		{
			Search(1,nullptr);
			return true;
		}
		case KEY_ALTF7:
		case KEY_RALTF7:
		{
			Search(-1,nullptr);
			return true;
		}
		case KEY_F8:
		{
			m_Codepage = f8cps.NextCP(m_Codepage);
			MB.SetCP(m_Codepage);
			lcache_ready = false;
			AdjustFilePos();
			ChangeViewKeyBar();
			Show();
			return true;
		}
		case KEY_SHIFTF8:
		{
			uintptr_t nCodePage = m_Codepage;
			if (codepages::instance().SelectCodePage(nCodePage, true, true, true))
			{
				if (nCodePage == CP_DEFAULT)
				{
					const auto DefaultCodepage = GetDefaultCodePage();
					const auto fpos = vtell();
					const auto DecectedCodepage = GetFileCodepage(ViewFile, DefaultCodepage, &Signature, true);
					vseek(fpos, FILE_BEGIN);
					nCodePage = IsCodePageSupported(DecectedCodepage)? DecectedCodepage : DefaultCodepage;
				}
				m_Codepage = nCodePage;
				MB.SetCP(m_Codepage);
				lcache_ready = false;
				AdjustFilePos();
				ChangeViewKeyBar();
				Show();
			}

			return true;
		}
		case KEY_ALTF8:
		case KEY_RALTF8:
		{
			if (ViewFile)
			{
				LastPage = false;
				GoTo();
			}

			return true;
		}
		case KEY_F11:
		{
			Global->CtrlObject->Plugins->CommandsMenu(windowtype_viewer,0,L"Viewer");
			Show();
			return true;
		}
		case KEY_MSWHEEL_UP:
		case(KEY_MSWHEEL_UP | KEY_ALT):
		case(KEY_MSWHEEL_UP | KEY_RALT):
		{
			int Roll = (LocalKey & (KEY_ALT|KEY_RALT))?1:(int)Global->Opt->MsWheelDeltaView;

			for (int i=0; i<Roll; i++)
				ProcessKey(Manager::Key(KEY_UP));

			return true;
		}
		case KEY_MSWHEEL_DOWN:
		case(KEY_MSWHEEL_DOWN | KEY_ALT):
		case(KEY_MSWHEEL_DOWN | KEY_RALT):
		{
			int Roll = (LocalKey & (KEY_ALT|KEY_RALT))?1:(int)Global->Opt->MsWheelDeltaView;

			for (int i=0; i<Roll; i++)
				ProcessKey(Manager::Key(KEY_DOWN));

			return true;
		}
		case KEY_MSWHEEL_LEFT:
		case(KEY_MSWHEEL_LEFT | KEY_ALT):
		case(KEY_MSWHEEL_LEFT | KEY_RALT):
		{
			int Roll = (LocalKey & (KEY_ALT|KEY_RALT))?1:(int)Global->Opt->MsHWheelDeltaView;

			for (int i=0; i<Roll; i++)
				ProcessKey(Manager::Key(KEY_LEFT));

			return true;
		}
		case KEY_MSWHEEL_RIGHT:
		case(KEY_MSWHEEL_RIGHT | KEY_ALT):
		case(KEY_MSWHEEL_RIGHT | KEY_RALT):
		{
			int Roll = (LocalKey & (KEY_ALT|KEY_RALT))?1:(int)Global->Opt->MsHWheelDeltaView;

			for (int i=0; i<Roll; i++)
				ProcessKey(Manager::Key(KEY_RIGHT));

			return true;
		}
		case KEY_UP: case KEY_NUMPAD8: case KEY_SHIFTNUMPAD8:
		{
			if (FilePos>0 && ViewFile)
			{
				Up(1, false); // LastPage = 0

				if (m_DisplayMode == VMT_TEXT)
				{
					ShowPage(SHOW_UP);
					ViewerString& end = Strings.back();
					LastPage = end.nFilePos >= FileSize || (end.eol_length == 0 && end.nFilePos + end.linesize >= FileSize);
				}
				else
				{
					Show();
				}
			}

			return true;
		}
		case KEY_DOWN: case KEY_NUMPAD2:  case KEY_SHIFTNUMPAD2:
		{
			if (!LastPage && ViewFile)
			{
				if (m_DisplayMode == VMT_TEXT)
				{
					ShowPage(SHOW_DOWN);

				}
				else
				{
					FilePos=SecondPos;
					Show();
				}
			}

			return true;
		}
		case KEY_PGUP: case KEY_NUMPAD9: case KEY_SHIFTNUMPAD9: case KEY_CTRLUP: case KEY_RCTRLUP:
		{
			if (ViewFile)
			{
				Up(m_Where.height() - 1, false);
				Show();
			}

			return true;
		}
		case KEY_PGDN: case KEY_NUMPAD3:  case KEY_SHIFTNUMPAD3: case KEY_CTRLDOWN: case KEY_RCTRLDOWN:
		{
			if (LastPage || !ViewFile)
				return true;

			FilePos = EndOfScreen(-1); // start of last screen line

			if (LocalKey == KEY_CTRLDOWN || LocalKey == KEY_RCTRLDOWN)
			{
				vseek(vString.nFilePos = FilePos, FILE_BEGIN);
				for (int i = m_Where.top; i <= m_Where.bottom; ++i)
				{
					ReadString(&vString,-1);
					vString.nFilePos += vString.linesize;
				}

				if (LastPage)
				{
					InternalKey++;
					ProcessKey(Manager::Key(KEY_CTRLPGDN));
					InternalKey--;
					return true;
				}
			}

			Show();
			return true;
		}
		case KEY_LEFT: case KEY_NUMPAD4: case KEY_SHIFTNUMPAD4:
		{
			if (LeftPos>0 && ViewFile)
			{
				if (m_DisplayMode == VMT_HEX && LeftPos > 80 - Width)
					LeftPos=std::max(80-Width,1);

				LeftPos--;
				Show();
			}

			return true;
		}
		case KEY_RIGHT: case KEY_NUMPAD6: case KEY_SHIFTNUMPAD6:
		{
			if (LeftPos < static_cast<int>(MaxViewLineSize()) && ViewFile && m_DisplayMode == VMT_TEXT && !m_Wrap)
			{
				LeftPos++;
				Show();
			}

			return true;
		}
		case KEY_CTRLLEFT:  case KEY_CTRLNUMPAD4:
		case KEY_RCTRLLEFT: case KEY_RCTRLNUMPAD4:
		{
			if (ViewFile)
			{
				if (m_DisplayMode == VMT_TEXT)
				{
					LeftPos = LeftPos > 20? LeftPos - 20 : 0;
				}
				else
				{
					const auto CharSize = GetModeDependentCharSize();
					FilePos = FilePos > CharSize? FilePos - CharSize : 0;
					FilePos -= FilePos % CharSize;
				}

				Show();
			}

			return true;
		}
		case KEY_CTRLRIGHT:  case KEY_CTRLNUMPAD6:
		case KEY_RCTRLRIGHT: case KEY_RCTRLNUMPAD6:
		{
			if (ViewFile)
			{
				if (m_DisplayMode == VMT_TEXT)
				{
					if (!m_Wrap)
					{
						LeftPos = std::min(LeftPos + 20, static_cast<long long>(MaxViewLineSize()));
					}
				}
				else
				{
					const auto CharSize = GetModeDependentCharSize();
					FilePos -= FilePos % CharSize;
					FilePos = FilePos < FileSize - CharSize? FilePos + CharSize : FileSize-1;
					FilePos -= FilePos % CharSize;
				}

				Show();
			}

			return true;
		}
		case KEY_CTRLSHIFTLEFT:    case KEY_CTRLSHIFTNUMPAD4:
		case KEY_RCTRLSHIFTLEFT:   case KEY_RCTRLSHIFTNUMPAD4:
		{
			// Перейти на начало строк
			if (ViewFile)
			{
				LeftPos = 0;
				Show();
			}

			return true;
		}
		case KEY_CTRLSHIFTRIGHT:     case KEY_CTRLSHIFTNUMPAD6:
		case KEY_RCTRLSHIFTRIGHT:    case KEY_RCTRLSHIFTNUMPAD6:
		{
			// Перейти на конец строк
			if (ViewFile)
			{
				const size_t MaxLen = std::accumulate(ALL_CONST_RANGE(Strings), size_t(0), [](size_t Value, const ViewerString& i)
				{
					return std::max(Value, i.Data.size());
				});
				LeftPos = (MaxLen > static_cast<size_t>(Width))? MaxLen - Width : 0;
				Show();
			}

			return true;
		}

		case KEY_CTRLHOME:    case KEY_CTRLNUMPAD7:
		case KEY_RCTRLHOME:   case KEY_RCTRLNUMPAD7:
		case KEY_HOME:        case KEY_NUMPAD7:   case KEY_SHIFTNUMPAD7:
			// Перейти на начало файла
			if (ViewFile)
				LeftPos=0;
			[[fallthrough]];
		case KEY_CTRLPGUP:    case KEY_CTRLNUMPAD9:
		case KEY_RCTRLPGUP:   case KEY_RCTRLNUMPAD9:
			if (ViewFile)
			{
				FilePos=0;
				Show();
			}
			return true;

		case KEY_CTRLEND:     case KEY_CTRLNUMPAD1:
		case KEY_RCTRLEND:    case KEY_RCTRLNUMPAD1:
		case KEY_END:         case KEY_NUMPAD1: case KEY_SHIFTNUMPAD1:
			// Перейти на конец файла
			if (ViewFile)
				LeftPos=0;
			[[fallthrough]];
		case KEY_CTRLPGDN:    case KEY_CTRLNUMPAD3:
		case KEY_RCTRLPGDN:   case KEY_RCTRLNUMPAD3:
			if (ViewFile)
			{
				int max_counter = m_Where.height() - 1;
				const auto CharSize = getChSize(m_Codepage);

				if (m_DisplayMode == VMT_TEXT)
				{
					vseek(0, FILE_END);
					FilePos = vtell();
					if (FilePos > 0)
					{
						--FilePos;
						FilePos -= FilePos % CharSize;
						vseek(FilePos, FILE_BEGIN);
						wchar_t LastSym = L'\0';
						if (vgetc(&LastSym) && !IsEol(LastSym))
							++max_counter;

						FilePos = vtell();
					}
					Up(max_counter, false);
				}
				else
				{
					const auto LineSize = GetModeDependentLineSize();
					vseek(0, FILE_END);
					FilePos = vtell();
					FilePos -= FilePos % CharSize;
					if ((FilePos % LineSize) == 0)
						FilePos -= LineSize * m_Where.height();
					else
						FilePos -= (FilePos % LineSize) + LineSize * (m_Where.height() - 1);
					if (FilePos < 0)
						FilePos = 0;
				}

				Show();
			}

			return true;
		default:

			if (LocalKey >= ' ' && IsCharKey(LocalKey))
			{
				Search(0,&Key);
				return true;
			}
	}

	return false;
}

bool Viewer::ProcessMouse(const MOUSE_EVENT_RECORD *MouseEvent)
{
	if (!(MouseEvent->dwButtonState & 3))
		return false;

	if (ViOpt.ShowScrollbar && IntKeyState.MousePos.x == m_Where.right + (m_bQuickView? 1 : 0))
	{
		if (IntKeyState.MousePos.y == m_Where.top)
		{
			// Press and hold the [▲] button
			while_mouse_button_pressed([&]
			{
				ProcessKey(Manager::Key(KEY_UP));
				return true;
			});
		}
		else if (IntKeyState.MousePos.y == m_Where.bottom)
		{
			// Press and hold the [▼] button
			while_mouse_button_pressed([&]
			{
				ProcessKey(Manager::Key(KEY_DOWN));
				return true;
			});
		}
		else if (IntKeyState.MousePos.y == m_Where.top + 1)
		{
			// Top click approximation
			ProcessKey(Manager::Key(KEY_CTRLHOME));
		}
		else if (IntKeyState.MousePos.y == m_Where.bottom - 1)
		{
			// Bottom click approximation
			ProcessKey(Manager::Key(KEY_CTRLEND));
		}
		else
		{
			// Drag the thumb
			while (IsMouseButtonPressed())
			{
				FilePos = (FileSize - 1) / (m_Where.height() - 2) * (IntKeyState.MousePos.y - m_Where.top);
				int Perc;

				if (FilePos > FileSize)
				{
					FilePos=FileSize;
					Perc=100;
				}
				else if (FilePos < 0)
				{
					FilePos=0;
					Perc=0;
				}
				else
					Perc=ToPercent(FilePos,FileSize);

				if (Perc == 100)
					ProcessKey(Manager::Key(KEY_CTRLEND));
				else if (!Perc)
					ProcessKey(Manager::Key(KEY_CTRLHOME));
				else
				{
					AdjustFilePos();
					Show();
				}
			}
		}

		return true;
	}

	if (IntKeyState.MousePos.y == m_Where.top - 1 && (HostFileViewer && HostFileViewer->IsTitleBarVisible()))
	{
		while (IsMouseButtonPressed()) {}
		if (IntKeyState.MousePos.y != m_Where.top - 1)
			return true;

		int NameLen = std::max(20, ObjWidth()-40-(Global->Opt->ViewerEditorClock && HostFileViewer && HostFileViewer->IsFullScreen()? 3 + static_cast<int>(Global->CurrentTime.size()) : 0));
		int cp_len = static_cast<int>(str(m_Codepage).size());
		//                           ViewMode     CopdePage             Goto
		static const int keys[]   = {KEY_SHIFTF4, KEY_SHIFTF8,          KEY_ALTF8   };
		int xpos[std::size(keys)] = {NameLen,     NameLen+3+(5-cp_len), NameLen+40-4};
		int xlen[std::size(keys)] = {3,           cp_len,                          4};

		for (int i = 0; i < static_cast<int>(std::size(keys)); ++i)
		{
			if (IntKeyState.MousePos.x >= xpos[i] && IntKeyState.MousePos.x < xpos[i]+xlen[i])
			{
				ProcessKey(Manager::Key(keys[i]));
				return true;
			}
		}
	}

	if (!m_Where.contains(IntKeyState.MousePos))
		return false;

	if (GetAsyncKeyState(VK_SHIFT)<0 && GetAsyncKeyState(VK_CONTROL)>=0 && GetAsyncKeyState(VK_MENU)>=0)
	{
		long long filepos = XYfilepos(IntKeyState.MousePos.x - m_Where.left, IntKeyState.MousePos.y - m_Where.top), mpos = -1;
		if (filepos < 0)
			return false;

		if (ManualSelectPos < 0)
			ManualSelectPos = mpos = filepos;
		else if (filepos < ManualSelectPos)
		{
			using std::swap;
			swap(filepos, ManualSelectPos);
		}

		vseek(filepos, FILE_BEGIN);
		wchar_t ch;
		vgetc(&ch);
		SelectSize = vtell() - (SelectPos = ManualSelectPos);

		ManualSelectPos = mpos;
		Show();
		return true;
	}

	const auto DoKey = [&](int Key)
	{
		ProcessKey(Manager::Key(Key));
	};

	while_mouse_button_pressed([&]
	{
		if (IntKeyState.MousePos.x < m_Where.left + 7)
			DoKey(KEY_LEFT);
		else if (IntKeyState.MousePos.x > m_Where.right - 7)
			DoKey(KEY_RIGHT);

		DoKey(IntKeyState.MousePos.y < m_Where.top + (m_Where.height() - 1) / 2? KEY_UP : KEY_DOWN);
		return true;
	});

	return true;
}


void Viewer::CacheLine( long long start, int length, bool have_eol )
{
	assert(start >= 0 && length >= 0);
	if (!length) // empty lines beyond EOF
		return;

	if ( lcache_ready
		&& (lcache_wrap != static_cast<int>(m_Wrap) || lcache_wwrap != static_cast<int>(m_WordWrap) || lcache_width != Width)
	){
		lcache_ready = false;
	}

	if (!lcache_ready || start > lcache_last || start+length < lcache_first)
	{
		lcache_first = start;
		lcache_last = start + length;

		lcache_count = 2;
		lcache_base = 0;
		lcache_lines[0] = (have_eol ? -start : +start);
		lcache_lines[1] = start + length;

		lcache_wrap = m_Wrap; lcache_wwrap = m_WordWrap; lcache_width = Width;
		lcache_ready = true;
	}
	else if (start == lcache_last)
	{
		int i = (lcache_base + lcache_count - 1) % lcache_lines.size();
		lcache_lines[i] = (have_eol ? -start : +start);
		i = (i + 1) % lcache_lines.size();
		lcache_lines[i]	= lcache_last = start + length;
		if (static_cast<size_t>(lcache_count) < lcache_lines.size())
			++lcache_count;
		else
		{
			lcache_base = (lcache_base + 1) % lcache_lines.size(); // ++start
			lcache_first = llabs(lcache_lines[lcache_base]);
		}
	}
	else if (start+length == lcache_first)
	{
		lcache_base = static_cast<int>((lcache_base + lcache_lines.size() - 1) % lcache_lines.size()); // --start
		lcache_lines[lcache_base] = (have_eol ? -start : +start);
		lcache_first = start;
		if (static_cast<size_t>(lcache_count) < lcache_lines.size())
			++lcache_count;
		else
		{
			size_t i = (lcache_base + lcache_lines.size() - 1) % lcache_lines.size(); // i = start - 1
			lcache_last = llabs(lcache_lines[i]);
		}
	}
	else
	{
		bool reset = (start < lcache_first || start+length > lcache_last);
		if ( reset )
		{
			int i = CacheFindUp(start+length);
			reset = (i < 0 || llabs(lcache_lines[i]) != start);
			if ( !reset )
			{
				int j = (i + 1) % lcache_lines.size();
				reset = (llabs(lcache_lines[j]) != start+length);
			}
		}
#if defined(_DEBUG) && 0 // it is legal case if file changed...
		assert( !reset );
#endif
		if ( reset )
		{
			lcache_first = start;
			lcache_last = start + length;
			lcache_count = 2;
			lcache_base = 0;
			lcache_lines[0] = (have_eol ? -start : +start);
			lcache_lines[1] = start + length;
		}
	}
}

int Viewer::CacheFindUp( long long start )
{
	if ( lcache_ready
		&& (lcache_wrap != static_cast<int>(m_Wrap) || lcache_wwrap != static_cast<int>(m_WordWrap) || lcache_width != Width)
	){
		lcache_ready = false;
	}
	if ( !lcache_ready || start <= lcache_first || start > lcache_last )
		return -1;

	int i1 = 0, i2 = lcache_count - 1;
	for (;;)
	{
		if ( i1+1 >= i2 )
			return (lcache_base + i1) % lcache_lines.size();

		int i = (i1 + i2) / 2;
		int j = (lcache_base + i) % lcache_lines.size();
		if (llabs(lcache_lines[j]) < start)
			i1 = i;
		else
			i2 = i;
	}
}

static const int portion_size = 250;

template<typename T, typename F>
static int process_back(int BufferSize, int pos, long long& fpos, const F& Reader, const raw_eol& eol)
{
	T Buffer[portion_size/sizeof(T)];
	int nr = Reader({ Buffer, static_cast<size_t>(BufferSize) });

	if (nr != static_cast<int>(BufferSize / sizeof(T)))
	{
		throw MAKE_FAR_EXCEPTION(L"Wrong size"sv);
	}

	if (!pos)
	{
		const auto PopEol = [&](T Char) { return nr && Buffer[nr - 1] == Char && --nr; };

		if (PopEol(eol.lf<T>()))
		{
			if (PopEol(eol.cr<T>()))
			{
				PopEol(eol.cr<T>());
			}
		}
		else
		{
			PopEol(eol.cr<T>());
		}
	}

	const T crlf[] = { eol.cr<T>(), eol.lf<T>() };
	const auto REnd = std::make_reverse_iterator(Buffer);
	const auto RBegin = REnd - nr;
	const auto Iterator = std::find_first_of(RBegin, REnd, ALL_CONST_RANGE(crlf));
	if (Iterator != REnd)
	{
		fpos += sizeof(T) * (REnd - Iterator);
		return true;
	}
	return false;
}

void Viewer::Up( int nlines, bool adjust )
{
	assert( nlines > 0 );

	if (!ViewFile)
		return;

	LastPage = false;

	if (FilePos <= 0)
		return;

	if (m_DisplayMode != VMT_TEXT)
	{
		const long long LineSize = GetModeDependentLineSize();
		FilePos = FilePos > LineSize * nlines? FilePos - LineSize * nlines : 0;
		return;
	}

	long long fpos = FilePos;

	int i = CacheFindUp(fpos);
	if ( i >= 0 )
	{
		for (;;)
		{
			fpos = llabs(lcache_lines[i]);
			if (--nlines == 0)
			{
				FilePos = fpos;
				return;
			}
			if (i == lcache_base)
				break;
			i = static_cast<int>((i + lcache_lines.size() - 1) % lcache_lines.size());
		}
	}

	int ch_size = getCharSize();

	const raw_eol eol;

	while ( nlines > 0 )
	{
		if ( fpos <= 0 )
		{
			FilePos = 0;
			return;
		}

		long long fpos1 = fpos;

		// backward CR-LF search
		//
		for (int j = 0; j < max_backward_size/portion_size; ++j )
		{
			int buff_size = (fpos > (long long)portion_size ? portion_size : (int)fpos);
			if ( buff_size <= 0 )
				break;
			fpos -= buff_size;
			vseek(fpos, FILE_BEGIN);

			if ( ch_size <= 1 )
			{
				const auto BufferReader = [&](range<char*> Buffer)
				{
					size_t nread = 0;
					Reader.Read(Buffer.data(), buff_size, &nread);
					return static_cast<int>(nread);
				};
				try
				{
					if (process_back<char>(buff_size, j, fpos, BufferReader, eol))
						break;
				}
				catch (const far_exception&)
				{
					return; //??? error handling
				}
			}
			else
			{
				const auto BufferReader = [&](range<wchar_t*> Buffer)
				{
					return vread(Buffer.data(), static_cast<int>(Buffer.size()));
				};
				try
				{
					if (process_back<wchar_t>(buff_size, j, fpos, BufferReader, eol))
						break;
				}
				catch (const far_exception&)
				{
					return; //??? error handling
				}
			}
		}

		// split read portion
		//
		vseek(vString.nFilePos = fpos, FILE_BEGIN);
		for (i = 0; i < static_cast<int>(llengths.size()); ++i)
		{
			ReadString(&vString, -1, false);
			llengths[i] = (vString.eol_length != 0 ? -1 : +1) * vString.linesize;
			if ((vString.nFilePos += vString.linesize) >= fpos1)
			{
				if (adjust)
					fpos1 = vString.nFilePos;
				else
					llengths[i] = vString.linesize - static_cast<int>(vString.nFilePos - fpos1);
				break;
			}
		}
		assert(i < static_cast<int>(llengths.size()));
		if (i >= static_cast<int>(llengths.size()))
			--i;

		while ( i >= 0 )
		{
			int l = llengths[i--];
			bool IsEol = false;
			if (l < 0)
			{
				IsEol = true;
				l = -l;
			}
			fpos1 -= l;
			CacheLine(fpos1, l, IsEol);
			if (--nlines == 0)
				FilePos = fpos1;
		}
	}
}

int Viewer::GetStrBytesNum(string_view const Str) const
{
	const auto ch_size = getCharSize();
	if (ch_size > 0)
		return static_cast<int>(Str.size() * ch_size);

	return static_cast<int>(encoding::get_bytes_count(m_Codepage, Str));
}

void Viewer::SetViewKeyBar(KeyBar *ViewKeyBar)
{
	m_ViewKeyBar = ViewKeyBar;
	ChangeViewKeyBar();
}

void Viewer::UpdateViewKeyBar(KeyBar& keybar)
{
	string f2_label, shiftf2_label;
	if (m_DisplayMode == VMT_TEXT)
	{
		f2_label = msg(m_Wrap? lng::MViewF2Unwrap : m_WordWrap? lng::MViewShiftF2 : lng::MViewF2);
		shiftf2_label = msg(m_WordWrap? lng::MViewF2 : lng::MViewShiftF2);
	}
	else
		f2_label = msg(m_DisplayMode == VMT_DUMP || m_DumpTextMode ? lng::MViewF4Text : lng::MViewF4Dump);

	keybar[KBL_MAIN][F2] = f2_label;
	keybar[KBL_SHIFT][F2] = shiftf2_label;

	keybar[KBL_MAIN][F4] = msg(m_DisplayMode != VMT_HEX? lng::MViewF4 : (m_DumpTextMode? lng::MViewF4Dump : lng::MViewF4Text));
	keybar[KBL_MAIN][F8] = f8cps.NextCPname(m_Codepage);
}

void Viewer::ChangeViewKeyBar()
{
	if (m_ViewKeyBar)
	{
		UpdateViewKeyBar(*m_ViewKeyBar);
		m_ViewKeyBar->Redraw();
	}
}

enum SEARCHDLG
{
	SD_DOUBLEBOX,
	SD_TEXT_SEARCH,
	SD_EDIT_TEXT,
	SD_EDIT_HEX,
	SD_SEPARATOR1,
	SD_RADIO_TEXT,
	SD_RADIO_HEX,
	SD_CHECKBOX_CASE,
	SD_CHECKBOX_WORDS,
	SD_CHECKBOX_REVERSE,
	SD_CHECKBOX_REGEXP,
	SD_SEPARATOR2,
	SD_BUTTON_OK,
	SD_BUTTON_CANCEL,
};

enum
{
	DM_SDSETVISIBILITY = DM_USER + 1,
};

struct ViewerDialogData
{
	Viewer* viewer;
	bool edit_autofocus;
	bool hex_mode;
	bool recursive;
};

intptr_t Viewer::ViewerSearchDlgProc(Dialog* Dlg,intptr_t Msg,intptr_t Param1,void* Param2)
{
	switch (Msg)
	{
		case DN_INITDIALOG:
		{
			Dlg->SendMessage(DM_SDSETVISIBILITY, Dlg->SendMessage(DM_GETCHECK, SD_RADIO_HEX, nullptr) == BSTATE_CHECKED, nullptr);
			Dlg->SendMessage(DM_EDITUNCHANGEDFLAG,SD_EDIT_TEXT,ToPtr(1));
			Dlg->SendMessage(DM_EDITUNCHANGEDFLAG,SD_EDIT_HEX,ToPtr(1));
			return TRUE;
		}
		case DM_SDSETVISIBILITY:
		{
			Dlg->SendMessage(DM_SHOWITEM,SD_EDIT_TEXT,ToPtr(!Param1));
			Dlg->SendMessage(DM_SHOWITEM,SD_EDIT_HEX,ToPtr(Param1));
			Dlg->SendMessage(DM_ENABLE,SD_CHECKBOX_CASE,ToPtr(!Param1));
			int re = Dlg->SendMessage(DM_GETCHECK, SD_CHECKBOX_REGEXP, nullptr) == BSTATE_CHECKED;
			int ww = !Param1 && !re;
			Dlg->SendMessage(DM_ENABLE,SD_CHECKBOX_WORDS,ToPtr(ww));
			Dlg->SendMessage(DM_ENABLE,SD_CHECKBOX_REGEXP,ToPtr(!Param1));
			return TRUE;
		}
		case DN_KILLFOCUS:
		{
			if (SD_EDIT_TEXT == Param1 || SD_EDIT_HEX == Param1)
			{
				const auto Data = reinterpret_cast<ViewerDialogData*>(Dlg->SendMessage(DM_GETITEMDATA, SD_EDIT_TEXT, nullptr));
				Data->hex_mode = (SD_EDIT_HEX == Param1);
			}
			break;
		}
		case DN_BTNCLICK:
		{
			bool need_focus = false;
			const auto Data = reinterpret_cast<ViewerDialogData*>(Dlg->SendMessage(DM_GETITEMDATA, SD_EDIT_TEXT, nullptr));
			int cradio = (Data->hex_mode ? SD_RADIO_HEX : SD_RADIO_TEXT);

			if ((Param1 == SD_RADIO_TEXT || Param1 == SD_RADIO_HEX) && Param2)
			{
				need_focus = true;
				if ( Param1 != cradio)
				{
					bool new_hex = (Param1 == SD_RADIO_HEX);

					Dlg->SendMessage(DM_ENABLEREDRAW, FALSE, nullptr);

					int sd_dst = new_hex ? SD_EDIT_HEX : SD_EDIT_TEXT;
					int sd_src = new_hex ? SD_EDIT_TEXT : SD_EDIT_HEX;

					EditorSetPosition esp={sizeof(EditorSetPosition)};
					esp.CurPos = -1;
					Dlg->SendMessage(DM_GETEDITPOSITION, sd_src, &esp);
					FarDialogItemData item = {sizeof(FarDialogItemData)};
					Dlg->SendMessage(DM_GETTEXT, sd_src, &item);
					const string Src(reinterpret_cast<const wchar_t*>(Dlg->SendMessage(DM_GETCONSTTEXTPTR, sd_src, nullptr)), item.PtrLength);
					const auto strTo = ConvertHexString(Src, m_Codepage, !new_hex);
					item.PtrLength = strTo.size();
					item.PtrData = UNSAFE_CSTR(strTo);
					Dlg->SendMessage(DM_SETTEXT, sd_dst, &item);
					Dlg->SendMessage(DM_SDSETVISIBILITY, new_hex, nullptr);
					if (esp.CurPos >= 0)
					{
						int p = esp.CurPos;
						if (Dlg->SendMessage(DM_GETEDITPOSITION, sd_dst, &esp))
						{
							esp.CurPos = esp.CurTabPos = p;
							esp.LeftPos = 0;
							Dlg->SendMessage(DM_SETEDITPOSITION, sd_dst, &esp);
						}
					}

					if (!strTo.empty())
					{
						int changed = (int)Dlg->SendMessage(DM_EDITUNCHANGEDFLAG, sd_src, ToPtr(-1));
						Dlg->SendMessage(DM_EDITUNCHANGEDFLAG, sd_dst, ToPtr(changed));
					}

					Dlg->SendMessage(DM_ENABLEREDRAW, TRUE, nullptr);
					Data->hex_mode = new_hex;
					if (!Data->edit_autofocus)
						return TRUE;
				}
			}
			else if (Param1 == SD_CHECKBOX_REGEXP)
			{
				Dlg->SendMessage(DM_SDSETVISIBILITY, Data->hex_mode, nullptr);
			}

			if (Data->edit_autofocus && !Data->recursive)
			{
				if ( need_focus
				  || Param1 == SD_CHECKBOX_CASE
				  || Param1 == SD_CHECKBOX_WORDS
				  || Param1 == SD_CHECKBOX_REVERSE
				  || Param1 == SD_CHECKBOX_REGEXP
				){
					Data->recursive = true;
					Dlg->SendMessage(DM_SETFOCUS, Data->hex_mode? SD_EDIT_HEX : SD_EDIT_TEXT, nullptr);
					Data->recursive = false;
				}
			}

			if (need_focus)
				return TRUE;
			else
				break;
		}
		case DN_HOTKEY:
		{
			if (Param1==SD_TEXT_SEARCH)
			{
				const auto Data = reinterpret_cast<const ViewerDialogData*>(Dlg->SendMessage(DM_GETITEMDATA, SD_EDIT_TEXT, nullptr));
				Dlg->SendMessage(DM_SETFOCUS, (Data->hex_mode? SD_EDIT_HEX : SD_EDIT_TEXT), nullptr);
				return FALSE;
			}
			break;
		}
		case DN_DRAWDIALOGDONE:
		{
			auto FirstChar = reinterpret_cast<const Manager::Key*>(Dlg->SendMessage(DM_SETDLGDATA, 0, nullptr));
			if (FirstChar)
				Global->WindowManager->CallbackWindow([Dlg, FirstChar]() { Dlg->ProcessKey(*FirstChar); });
			break;
		}
		default:
			break;
	}

	return Dlg->DefProc(Msg,Param1,Param2);
}

struct ViewerPreRedrawItem : public PreRedrawItem
{
	ViewerPreRedrawItem():
		PreRedrawItem(PR_ViewerSearchMsg),
		percent(),
		hex()
	{}

	string name;
	int percent;
	int hex;
};

static void ViewerSearchMsgImpl(const string& MsgStr, int Percent, int SearchHex)
{
	string strProgress;
	const auto strMsg = concat(msg(SearchHex? lng::MViewSearchingHex : lng::MViewSearchingFor), L' ', MsgStr);
	if (Percent>=0)
	{
		const size_t Length = std::max(std::min(ScrX - 1 - 10, static_cast<int>(strMsg.size())), 40);
		strProgress = make_progressbar(Length, Percent, true, true);
	}

	{
		std::vector<string> MsgItems{ strMsg };
		if (!strProgress.empty())
			MsgItems.emplace_back(strProgress);

		Message(MSG_LEFTALIGN,
			msg(lng::MViewSearchTitle),
			std::move(MsgItems),
			{});
	}
}

static void ViewerSearchMsg(const string& MsgStr, int Percent, int SearchHex)
{
	ViewerSearchMsgImpl(MsgStr, Percent, SearchHex);

	TPreRedrawFunc::instance()([&](ViewerPreRedrawItem& Item)
	{
		Item.name = MsgStr;
		Item.percent = Percent;
		Item.hex = SearchHex;
	});
}

static void PR_ViewerSearchMsg()
{
	TPreRedrawFunc::instance()([](const ViewerPreRedrawItem& Item)
	{
		ViewerSearchMsgImpl(Item.name, Item.percent, Item.hex);
	});
}

static auto hex2ss(const string_view from, intptr_t * const pos = nullptr)
{
	if (pos)
		*pos /= 2;
	return HexStringToBlob(trim_right(from), 0);
}

struct Viewer::search_data
{
	long long CurPos;
	long long MatchPos;
	const char *search_bytes;
	const wchar_t *search_text;
	int search_len;
	int  ch_size;
	bool first_Rex;
	int RexMatchCount;
	std::vector<RegExpMatch> RexMatch;
	std::unique_ptr<RegExp> pRex;

	search_data():
		CurPos(-1),
		MatchPos(-1),
		search_bytes(nullptr),
		search_text(nullptr),
		search_len(0),
		ch_size(0),
		first_Rex(true),
		RexMatchCount(0)
	{
	}

	int InitRegEx(const string& str, int flags)
	{
		pRex = std::make_unique<RegExp>();
		return pRex->Compile(str.c_str(), flags);
	}
};

enum SEARCHER_RESULT: int
{
	Search_NotFound  = 0,
	Search_Continue  = 1,
	Search_Eof       = 2,
	Search_Bof       = 3,
	Search_Cycle     = 4,
	Search_Found     = 5,
};

enum SEARCH_WRAP_MODE
{
	SearchWrap_NO    = 0,
	SearchWrap_END   = 1,
	SearchWrap_CYCLE = 2,
};

SEARCHER_RESULT Viewer::search_hex_forward(search_data* sd)
{
	char *buff = (char *)Search_buffer.data();
	const char *search_str = sd->search_bytes;
	int bsize = static_cast<int>(Search_buffer.size() * sizeof(wchar_t)), slen = sd->search_len;
	long long to, cpos = sd->CurPos;
	int swrap = ViOpt.SearchWrapStop;

	bool tail_part = cpos >= StartSearchPos;
	if (swrap == SearchWrap_CYCLE || tail_part)
	{
		if ( FileSize - cpos <= bsize )
			SetFileSize();
		to = FileSize;
	}
	else
	{
		to = StartSearchPos + slen - 1;
		if ( to > FileSize )
			to = FileSize;
	}

	int nb = (to - cpos < bsize ? static_cast<int>(to - cpos) : bsize);
	vseek(cpos, FILE_BEGIN);
	size_t nr = 0;
	Reader.Read(buff, nb, &nr);
	to = cpos + nr;
	int n1 = static_cast<int>(nr);
	if ( n1 != nb )
		SetFileSize();

	char *ps = buff;
	while (n1 >= slen)
	{
		const auto ps_end = ps + n1 - slen + 1;
		ps = std::find(ps, ps_end, search_str[0]);
		if (ps == ps_end)
			break;

		if (slen <= 1 || std::equal(ps + 1, ps + slen, search_str + 1))
		{
			sd->MatchPos = cpos + (ps - buff);
			return Search_Found;
		}
		++ps;
		n1 = static_cast<int>(nr - (ps - buff));
	}

	if ((LastSelectPos <= 0 && to >= FileSize) || (LastSelectPos > 0 && cpos < LastSelectPos && to >= LastSelectPos))
		return Search_NotFound;

	sd->CurPos = cpos + nr - slen + 1;
	if (sd->CurPos + slen > FileSize)
	{
		sd->CurPos = 0;
		if (swrap == SearchWrap_CYCLE && StartSearchPos == 0)
			return Search_Cycle;
		else if (swrap == SearchWrap_END)
			return Search_Eof;
		else
			return Search_Continue;
	}
	if (swrap == SearchWrap_CYCLE && !tail_part && sd->CurPos >= StartSearchPos)
		return Search_Cycle;
	else
		return Search_Continue;
}

SEARCHER_RESULT Viewer::search_hex_backward(search_data* sd)
{
	char *buff = (char *)Search_buffer.data();
	const char *search_str = sd->search_bytes;
	int bsize = static_cast<int>(Search_buffer.size() * sizeof(wchar_t)), slen = sd->search_len;
	long long to, cpos = sd->CurPos;
	int swrap = ViOpt.SearchWrapStop;

	bool lo_half = cpos <= StartSearchPos;
	if ( swrap != SearchWrap_CYCLE || lo_half )
	{
		to = 0;
	}
	else
	{
		to = StartSearchPos - slen + 1;
		if ( to < 0 )
			to = 0;
	}

	int nb = (cpos - to < bsize ? static_cast<int>(cpos - to) : bsize);
	vseek(to = cpos-nb, FILE_BEGIN);
	size_t nr = 0;
	Reader.Read(buff, static_cast<DWORD>(nb), &nr);
	int n1 = static_cast<int>(nr);
	if ( n1 != nb )
	{
		SetFileSize();
		cpos = vtell();
	}

	char *ps = buff + n1 - 1, last_char = search_str[slen-1];
	while (n1 >= slen)
	{
		if (*ps == last_char)
		{
			if (slen <= 1 || std::equal(ps - slen + 1, ps, search_str))
			{
				sd->MatchPos = cpos - nr + n1 - slen;
				return Search_Found;
			}
		}
		--ps;
		--n1;
	}

	if ((LastSelectPos >= FileSize && to <= 0) || (LastSelectPos > 0 && to <= LastSelectPos && cpos > LastSelectPos))
		return Search_NotFound;

	sd->CurPos = cpos - nr + slen - 1;
	if (sd->CurPos < slen)
	{
		sd->CurPos = FileSize;
		if (swrap == SearchWrap_CYCLE && StartSearchPos >= FileSize)
			return Search_Cycle;
		else if (swrap == SearchWrap_END)
			return Search_Bof;
		else
			return Search_Continue;
	}
	if (swrap == SearchWrap_CYCLE && !lo_half && sd->CurPos <= StartSearchPos)
		return Search_Cycle;
	else
		return Search_Continue;
}

SEARCHER_RESULT Viewer::search_text_forward(search_data* sd)
{
	int bsize = 8192, slen = sd->search_len, ww = (LastSearchWholeWords ? 1 : 0);
	wchar_t prev_char = L'\0', *buff = Search_buffer.data(), *t_buff = (sd->ch_size < 0 ? buff + bsize : nullptr);
	const wchar_t *search_str = sd->search_text;
	long long to, cpos = sd->CurPos;
	const auto swrap = ViOpt.SearchWrapStop;

	vseek(cpos, FILE_BEGIN);
	if ( ww )
		prev_char = vgetc_prev();

	const auto tail_part = cpos >= StartSearchPos;
	if (swrap != SearchWrap_CYCLE || tail_part )
	{
		if ( (to = FileSize) - cpos <= bsize )
			SetFileSize();
		to = FileSize;
	}
	else
	{
		to = StartSearchPos;
		if ( to > FileSize )
			to = FileSize;
	}

	int nb = (to - cpos > bsize ? bsize : static_cast<int>(to - cpos));
	int nw = vread(buff, nb, t_buff);
	auto to1 = vtell();
	if ( swrap == SearchWrap_CYCLE && !tail_part && nb + 3*(slen+ww) < bsize && !veof() )
	{
		int nw1 = vread(buff+nw, 3*(slen+ww), t_buff ? t_buff+nw : nullptr);
		nw1 = std::max(nw1, slen+ww-1);
		nw += nw1;
		to1 = to + (t_buff ? GetStrBytesNum({ t_buff, static_cast<size_t>(nw1) }) : sd->ch_size * nw1);
	}

	int is_eof = (to1 >= FileSize ? 1 : 0), iLast = nw - slen - ww + ww*is_eof;
	if ( !LastSearchCase )
		CharUpperBuff(buff, nw);

	for (int i = 0; i <= iLast; ++i)
	{
		if (ww)
		{
			if (!is_word_div(i > 0 ? buff[i-1] : prev_char))
				continue;
			if (!(i == iLast && is_eof) && !is_word_div(buff[i+slen]))
				continue;
		}
		if ( buff[i] != search_str[0]
		 || (slen > 1 && buff[i+1] != search_str[1])
		 || (slen > 2 && !std::equal(buff + i + 2, buff + i + slen, search_str + 2))
		) continue;

		sd->MatchPos = cpos + GetStrBytesNum({ buff, static_cast<size_t>(i) });
		sd->search_len = GetStrBytesNum({ buff + i, static_cast<size_t>(slen) });
		return Search_Found;
	}

	if (is_eof)
	{
		sd->CurPos = 0;

		if (LastSelectPos <= 0 || cpos < LastSelectPos)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && StartSearchPos == 0)
			return Search_Cycle;
		else if (swrap == SearchWrap_END)
			return Search_Eof;
	}
	else
	{
		sd->CurPos = to1 - GetStrBytesNum({ t_buff + iLast + 1, static_cast<size_t>(nw - iLast - 1) });

		if (LastSelectPos > 0 && cpos < LastSelectPos && sd->CurPos >= LastSelectPos)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && !tail_part && sd->CurPos >= StartSearchPos)
			return Search_Cycle;
	}
	return Search_Continue;
}

SEARCHER_RESULT Viewer::search_text_backward(search_data* sd)
{
	int bsize = 8192, slen = sd->search_len, ww = (LastSearchWholeWords ? 1 : 0);
	wchar_t *buff = Search_buffer.data(), *t_buff = (sd->ch_size < 0 ? buff + bsize : nullptr);
	const wchar_t *search_str = sd->search_text;
	long long cpos = sd->CurPos;
	const auto swrap = ViOpt.SearchWrapStop;

	const auto tail_part = cpos > StartSearchPos;
	const auto to = (tail_part && swrap == SearchWrap_CYCLE ? StartSearchPos : 0);

	int nb = (cpos - to > bsize ? bsize : static_cast<int>(cpos- to));
	if (tail_part && swrap == SearchWrap_CYCLE && nb + 3*(slen+ww) < bsize && cpos > nb)
	{
		if ( sd->ch_size > 0 )
		{
			nb += sd->ch_size * (slen + ww - 1);
			if (cpos < nb)
				nb = static_cast<int>(cpos);
		}
		else
		{
			long long to1 = cpos - nb - 3*(slen + ww - 1);
			if (to1 < 0)
				to1 = 0;
			int nb1 = static_cast<int>(cpos - nb - to1);
			vseek(to1, FILE_BEGIN);
			int nw1 = vread(buff, nb1, t_buff);
			if (nw1 > slen + ww - 1)
				nb1 = GetStrBytesNum({ t_buff + nw1 - (slen + ww - 1), static_cast<size_t>(slen + ww - 1) });
			nb += nb1;
		}
	}

	cpos -= nb;
	vseek(cpos, FILE_BEGIN);
	int nw = vread(buff, nb, t_buff);
	if (!LastSearchCase)
		CharUpperBuff(buff, nw);

	int is_eof = (veof() ? 1 : 0), iFirst = ww * (cpos > 0 ? 1 : 0), iLast = nw - slen - ww + ww*is_eof;
	for ( int i = iLast; i >= iFirst; --i )
	{
		if (ww)
		{
			if ( i > 0 && !is_word_div(buff[i-1]) )
				continue;
			if ( !(i == iLast && is_eof) && !is_word_div(buff[i+slen]) )
				continue;
		}
		if ( buff[i] != search_str[0]
		|| (slen > 1 && buff[i+1] != search_str[1])
		|| (slen > 2 && !std::equal(buff + i + 2, buff + i + slen, search_str + 2))
		) continue;

		sd->MatchPos = cpos + GetStrBytesNum({ t_buff, static_cast<size_t>(i) });
		sd->search_len = GetStrBytesNum({ t_buff + i, static_cast<size_t>(slen) });
		return Search_Found;
	}

	if (cpos <= 0) // bof()
	{
		SetFileSize();
		sd->CurPos = FileSize;

		if (LastSelectPos > FileSize || tail_part)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && StartSearchPos >= FileSize)
			return Search_Cycle;
		else if (swrap == SearchWrap_END)
			return Search_Bof;
	}
	else
	{
		sd->CurPos = cpos + GetStrBytesNum({ t_buff, static_cast<size_t>(iFirst + slen - 1) });

		if (cpos+nb > LastSelectPos && sd->CurPos <= LastSelectPos)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && tail_part && sd->CurPos <= StartSearchPos)
			return Search_Cycle;
	}
	return Search_Continue;
}

int Viewer::read_line(wchar_t *buf, wchar_t *tbuf, long long cpos, int adjust, long long& lpos, int &lsize)
{
	const auto OldFilePos = FilePos;
	const auto OldLastPage = LastPage;
	const auto OldWrap = m_Wrap;
	const auto OldWrapTouched = m_Wrap.touched();
	const auto OldWordWrap = m_WordWrap;
	const auto OldWordWrapTouched = m_WordWrap.touched();
	const auto OldDisplayMode = m_DisplayMode;

	m_DisplayMode = VMT_TEXT;
	m_Wrap = m_WordWrap = false;

	FilePos = cpos;
	if (adjust)
	{
		if (adjust > 0)
			AdjustFilePos();
		else
			Up(1, true);
	}

	vseek(lpos = vString.nFilePos = FilePos, FILE_BEGIN);
	vString.linesize = 0;
	ReadString(&vString, -1, false); // read unwrapped text line

	vseek(FilePos, FILE_BEGIN);
	int llen = vread(buf, lsize = vString.linesize, tbuf);
	if (llen > 0)
		llen -= vString.eol_length; // remove eol-s
	buf[llen >= 0 ? llen : 0] = L'\0';

	m_DisplayMode = OldDisplayMode;

	m_Wrap = OldWrap;
	m_WordWrap = OldWordWrap;
	if (!OldWrapTouched)
	{
		m_Wrap.forget();
	}
	if (!OldWordWrapTouched)
	{
		m_WordWrap.forget();
	}
	FilePos = OldFilePos;
	LastPage = OldLastPage;

	return llen;
}

SEARCHER_RESULT Viewer::search_regex_forward(search_data* sd)
{
	assert(sd->pRex);
	assert(Search_buffer.size() >= 2 * MaxViewLineBufferSize());

	wchar_t *line = Search_buffer.data(), *t_line = sd->ch_size < 0 ? Search_buffer.data() + MaxViewLineBufferSize() : nullptr;
	long long cpos = sd->CurPos, bpos = 0;

	int first = (sd->first_Rex ? +1 : 0);
	sd->first_Rex = false;
	bool tail_part = cpos >= StartSearchPos;
	int swrap = ViOpt.SearchWrapStop;

	int lsize = 0, nw = read_line(line, t_line, cpos, first, bpos, lsize);
	if (lsize <= 0) //eof() -- TODO: error handling
	{
		sd->CurPos = 0;
		if (!tail_part || StartSearchPos <= 0)
			return Search_NotFound;
		else
			return swrap == SearchWrap_END ? Search_Eof : Search_Continue;
	}

	int off = 0;
	for (;;)
	{
		if ( off > nw )
			break;

		intptr_t n = sd->RexMatchCount;
		RegExpMatch *m = sd->RexMatch.data();
		if (!sd->pRex->SearchEx(line, line + off, line + nw, m, n))  // doesn't match
		{
			ReMatchErrorMessage(*sd->pRex);
			break;
		}

		long long fpos = bpos + GetStrBytesNum({ t_line, static_cast<size_t>(m[0].start) });
		if ( fpos < cpos )
		{
			off = m[0].start + 1; // skip
			continue;
		}
		else if (swrap == SearchWrap_CYCLE && !tail_part && fpos >= StartSearchPos)
		{
			break; // done - not found
		}
		else // found
		{
			sd->MatchPos = fpos;
			sd->search_len = GetStrBytesNum({ t_line + off + m[0].start, static_cast<size_t>(m[0].end - m[0].start) });
			return Search_Found;
		}
	}

	if (veof())
	{
		sd->CurPos = 0;

		if (LastSelectPos <= 0 || cpos < LastSelectPos)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && StartSearchPos == 0)
			return Search_Cycle;
		else if (swrap == SearchWrap_END)
			return Search_Eof;
	}
	else
	{
		const auto pos = vtell();
		sd->CurPos = pos;

		if (LastSelectPos > 0 && cpos < LastSelectPos && pos >= LastSelectPos)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && !tail_part && pos >= StartSearchPos)
			return Search_Cycle;
	}
	return Search_Continue;
}

SEARCHER_RESULT Viewer::search_regex_backward(search_data* sd)
{
	assert(sd->pRex);
	assert(Search_buffer.size() >= 2 * MaxViewLineBufferSize());

	wchar_t *line = Search_buffer.data(), *t_line = sd->ch_size < 0 ? Search_buffer.data() + MaxViewLineBufferSize() : nullptr;
	long long cpos = sd->CurPos, bpos = 0, prev_pos = -1;

	const auto tail_part = cpos > StartSearchPos;
	const auto swrap = ViOpt.SearchWrapStop;

	int off=0, lsize=0, prev_len = -1;
	const auto nw = read_line(line, t_line, cpos, -1, bpos, lsize);
	for (;;)
	{
		if (lsize <= 0 || off > nw)
			break;

		intptr_t n = sd->RexMatchCount;
		RegExpMatch *m = sd->RexMatch.data();
		if (!sd->pRex->SearchEx(line, line + off, line + nw, m, n))
		{
			ReMatchErrorMessage(*sd->pRex);
			break;
		}

		long long fpos = bpos + GetStrBytesNum({ t_line, static_cast<size_t>(m[0].start) });
		int flen = GetStrBytesNum({ t_line + m[0].start, static_cast<size_t>(m[0].end - m[0].start) });
		if (fpos+flen > cpos)
			break;

		if (!(tail_part && fpos+flen <= StartSearchPos))
		{
			prev_pos = fpos;
			prev_len = flen;
		}

		off = m[0].start + 1; // skip
		continue;
	}

	if (prev_len >= 0)
	{
		sd->MatchPos = prev_pos;
		sd->search_len = prev_len;
		return Search_Found;
	}

	if (bpos <= 0) // bof()
	{
		SetFileSize();
		sd->CurPos = FileSize;

		if (LastSelectPos > FileSize || tail_part)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && StartSearchPos >= FileSize)
			return Search_Cycle;
		else if (swrap == SearchWrap_END)
			return Search_Bof;
	}
	else
	{
		sd->CurPos = bpos;

		if (cpos > LastSelectPos && bpos <= LastSelectPos)
			return Search_NotFound;
		else if (swrap == SearchWrap_CYCLE && tail_part && bpos <= StartSearchPos)
			return Search_Cycle;
	}
	return Search_Continue;
}

/*
 + Параметр Next может принимать значения:
 0 - Новый поиск
 1 - Продолжить поиск со следующей позиции
-1 - Продолжить поиск со следующей позиции в противоположном направлении
*/
void Viewer::Search(int Next,const Manager::Key* FirstChar)
{
	if (!ViewFile || (Next && strLastSearchStr.empty()))
		return;

	auto SearchHex = LastSearchHex;
	auto Case = LastSearchCase;
	auto WholeWords = LastSearchWholeWords;
	auto ReverseSearch = LastSearchReverse;
	auto SearchRegexp = LastSearchRegexp;

	string strSearchStr;
	if (!strLastSearchStr.empty())
		strSearchStr = strLastSearchStr;

	search_data sd;

	if (!Next)
	{
		FarDialogItem const SearchDlgData[]
		{
			{DI_DOUBLEBOX,3,1,72,11,0,nullptr,nullptr,0,msg(lng::MViewSearchTitle).c_str()},
			{DI_TEXT,5,2,0,2,0,nullptr,nullptr,0,msg(lng::MViewSearchFor).c_str()},
			{DI_EDIT,5,3,70,3,0,L"SearchText",nullptr,DIF_FOCUS|DIF_HISTORY|DIF_USELASTHISTORY,L""},
			{DI_FIXEDIT,5,3,70,3,0,nullptr,L"HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH ",DIF_MASKEDIT,L""},
			{DI_TEXT,-1,4,0,4,0,nullptr,nullptr,DIF_SEPARATOR,L""},
			{DI_RADIOBUTTON,5,5,0,5,1,nullptr,nullptr,DIF_GROUP,msg(lng::MViewSearchForText).c_str()},
			{DI_RADIOBUTTON,5,6,0,6,0,nullptr,nullptr,0,msg(lng::MViewSearchForHex).c_str()},
			{DI_CHECKBOX,40,5,0,5,0,nullptr,nullptr,0,msg(lng::MViewSearchCase).c_str()},
			{DI_CHECKBOX,40,6,0,6,0,nullptr,nullptr,0,msg(lng::MViewSearchWholeWords).c_str()},
			{DI_CHECKBOX,40,7,0,7,0,nullptr,nullptr,0,msg(lng::MViewSearchReverse).c_str()},
			{DI_CHECKBOX,40,8,0,8,0,nullptr,nullptr,DIF_DISABLE,msg(lng::MViewSearchRegexp).c_str()},
			{DI_TEXT,-1,9,0,9,0,nullptr,nullptr,DIF_SEPARATOR,L""},
			{DI_BUTTON,0,10,0,10,0,nullptr,nullptr,DIF_DEFAULTBUTTON|DIF_CENTERGROUP,msg(lng::MViewSearchSearch).c_str()},
			{DI_BUTTON,0,10,0,10,0,nullptr,nullptr,DIF_CENTERGROUP,msg(lng::MViewSearchCancel).c_str()},
		};
		auto SearchDlg = MakeDialogItemsEx(SearchDlgData);

		SearchDlg[SD_RADIO_TEXT].Selected=!LastSearchHex;
		SearchDlg[SD_RADIO_HEX].Selected=LastSearchHex;
		SearchDlg[SD_CHECKBOX_CASE].Selected=LastSearchCase;
		SearchDlg[SD_CHECKBOX_WORDS].Selected=LastSearchWholeWords;
		SearchDlg[SD_CHECKBOX_REVERSE].Selected=LastSearchReverse;
		SearchDlg[SD_CHECKBOX_REGEXP].Selected=LastSearchRegexp;
		SearchDlg[SearchDlg[SD_RADIO_HEX].Selected? SD_EDIT_HEX : SD_EDIT_TEXT].strData = strSearchStr;

		ViewerDialogData my;
		//
		my.viewer = this;
		my.edit_autofocus = (ViOpt.SearchEditFocus != 0);
		my.hex_mode = (LastSearchHex != 0);
		my.recursive = false;
		//
		SearchDlg[SD_EDIT_TEXT].UserData = (intptr_t)&my;

		const auto Dlg = Dialog::create(SearchDlg, &Viewer::ViewerSearchDlgProc, this, const_cast<Manager::Key*>(FirstChar));
		Dlg->SetId(ViewerSearchId);
		Dlg->SetHelp(L"ViewerSearch"sv);
		Dlg->SetPosition({ -1, -1, 76, 13 });

		Dlg->Process();

		if (Dlg->GetExitCode()!=SD_BUTTON_OK)
			return;

		SearchHex=SearchDlg[SD_RADIO_HEX].Selected == BSTATE_CHECKED;
		Case=SearchDlg[SD_CHECKBOX_CASE].Selected == BSTATE_CHECKED;
		WholeWords=SearchDlg[SD_CHECKBOX_WORDS].Selected == BSTATE_CHECKED;
		ReverseSearch=SearchDlg[SD_CHECKBOX_REVERSE].Selected == BSTATE_CHECKED;
		SearchRegexp=SearchDlg[SD_CHECKBOX_REGEXP].Selected == BSTATE_CHECKED;

		if (SearchHex)
		{
			strSearchStr = ExtractHexString(SearchDlg[SD_EDIT_HEX].strData);
		}
		else
		{
			strSearchStr = SearchDlg[SD_EDIT_TEXT].strData;
		}
	}

	LastSearchCase = Case;
	LastSearchWholeWords = WholeWords;
	LastSearchReverse = ReverseSearch;
	LastSearchRegexp = SearchRegexp;
	LastSearchHex = SearchHex;

	if (Next == -1)
		ReverseSearch = !ReverseSearch;

	auto strMsgStr = strLastSearchStr = strSearchStr;

	sd.search_len = static_cast<int>(strSearchStr.size());

	bytes search_bytes;
	decltype(&Viewer::search_hex_forward) searcher;

	if (SearchHex)
	{
		search_bytes = hex2ss(strSearchStr);
		sd.search_len = (int)search_bytes.size();
		sd.search_bytes = search_bytes.data();
		sd.ch_size = 1;
		Case = true;
		WholeWords = false;
		SearchRegexp = false;
		searcher = (ReverseSearch ? &Viewer::search_hex_backward : &Viewer::search_hex_forward);
	}
	else
	{
		sd.ch_size = getCharSize();
		sd.search_text = strSearchStr.c_str();

		if (SearchRegexp)
		{
			WholeWords = false;
			searcher = (ReverseSearch ? &Viewer::search_regex_backward : &Viewer::search_regex_forward);

			const auto strSlash = InsertRegexpQuote(strSearchStr);

			strMsgStr = strSlash;

			if (!sd.InitRegEx(strSlash, OP_PERLSTYLE | OP_OPTIMIZE | (Case ? 0 : OP_IGNORECASE)))
			{
				ReCompileErrorMessage(*sd.pRex, strSlash);
				return; // wrong regular expression...
			}
			sd.RexMatchCount = sd.pRex->GetBracketsCount();
			sd.RexMatch.resize(sd.RexMatchCount);
		}
		else
		{
			searcher = (ReverseSearch ? &Viewer::search_text_backward : &Viewer::search_text_forward);
			inplace::quote_unconditional(strMsgStr);
		}
	}

	if (!Case && !SearchRegexp)
	{
		inplace::upper(strSearchStr);
		sd.search_text = strSearchStr.c_str();
	}

	int search_direction = ReverseSearch ? -1 : +1;
	switch (Next)
	{
		case +1:
		case -1:
			if ( SelectPos >= 0 && SelectSize >= 0 )
			{
				if (sd.ch_size >= 1)
					LastSelectPos = SelectPos + (ReverseSearch ? LastSelectSize-sd.ch_size : sd.ch_size);
				else
				{
					long long prev_pos = SelectPos;
					vseek(SelectPos, FILE_BEGIN);
					for (;;)
					{
						wchar_t ch;
						bool ok_getc = vgetc(&ch);
						LastSelectPos = vtell();
						if (!ReverseSearch || !ok_getc)
							break;
						if ( LastSelectPos >= SelectPos + LastSelectSize )
						{
							LastSelectPos = prev_pos;
							break;
						}
						prev_pos = LastSelectPos;
					}
				}
				if (search_direction != LastSearchDirection)
					StartSearchPos = LastSelectPos;

				break;
			}
			[[fallthrough]];
		case 0:
		default:
			assert(Next >= -1 && Next <= +1);
			if (!Next || LastSelectSize < 0)
				LastSelectSize = SelectSize = -1;
			StartSearchPos = LastSelectPos = (ReverseSearch ? EndOfScreen(0) : BegOfScreen());
		break;
	}
	LastSearchDirection = search_direction;

	if (!sd.search_len || (long long)sd.search_len > FileSize)
		return;

	sd.CurPos = LastSelectPos;
	{
		SCOPED_ACTION(IndeterminateTaskbar);
		SCOPED_ACTION(TPreRedrawFuncGuard)(std::make_unique<ViewerPreRedrawItem>());
		SetCursorType(false, 0);

		const time_check TimeCheck(time_check::mode::delayed, GetRedrawTimeout());
		for (;;)
		{
			const auto found = std::invoke(searcher, this, &sd);
			if (found == Search_Found)
				break;

			else if (found == Search_Continue)
				;

			else if (found == Search_NotFound)
			{
				Message(MSG_WARNING,
					msg(lng::MViewSearchTitle),
					{
						msg(SearchHex? lng::MViewSearchCannotFindHex : lng::MViewSearchCannotFind),
						strMsgStr
					},
					{ lng::MOk });

				return;
			}
			else // Search_ Eof/Bof/Cycle
			{
				// MviewSearch Eod,FromBegin/Bod,FromEnd/Cycle,Repeat
				static_assert(Search_Bof - Search_Eof == 1 && Search_Cycle - Search_Eof == 2, "Wrong enum order");
				static_assert(lng::MViewSearchEod + 1 == lng::MViewSearchFromBegin, "Wrong .lng file order");
				static_assert(lng::MViewSearchBod + 1 == lng::MViewSearchFromEnd, "Wrong .lng file order");
				static_assert(lng::MViewSearchCycle + 1 == lng::MViewSearchRepeat, "Wrong .lng file order");
				static_assert(lng::MViewSearchEod + 2 == lng::MViewSearchBod && lng::MViewSearchEod + 4 == lng::MViewSearchCycle, "Wrong .lng file order");

				if (Message(0,
					msg(lng::MViewSearchTitle),
					{
						msg(lng::MViewSearchEod + 2 * (found - Search_Eof)),
						msg(lng::MViewSearchFromBegin + 2 * (found - Search_Eof)),
						strMsgStr
					},
					{ lng::MYes, lng::MCancel }) != Message::first_button) // cancel search
				{
					return;
				}
			}

			if (TimeCheck)
			{
				if (CheckForEscSilent())
				{
					if (ConfirmAbortOp())
					{
						Redraw();
						return;
					}
				}

				int percent = -1;
				long long total = FileSize;
				if ( total > 0 )
				{
					long long done;
					if ( !ReverseSearch )
					{
						if ( sd.CurPos >= StartSearchPos )
							done = sd.CurPos - StartSearchPos;
						else
							done = (FileSize - StartSearchPos) + sd.CurPos;
					}
					else
					{
						if ( sd.CurPos <= StartSearchPos )
							done = StartSearchPos - sd.CurPos;
						else
							done = StartSearchPos + (FileSize - sd.CurPos);
					}
					percent = static_cast<int>(done*100/total);
				}
				ViewerSearchMsg(strMsgStr, percent, SearchHex);
			}
		}
	}

	if ( sd.MatchPos >= 0 )
	{
		DWORD flags = ReverseSearch ? 0x2 : 0;

		if (sd.search_len < 0
		 || (sd.MatchPos >= BegOfScreen() && sd.MatchPos + sd.search_len <= EndOfScreen(0)))
		{
			SelectPos = sd.MatchPos;
			SelectSize = LastSelectSize = sd.search_len;
			SelectFlags = flags;
		}
		else
		{
			SelectText(sd.MatchPos, sd.search_len, flags);
			LastSelectSize = SelectSize;

			// Покажем найденное на расстоянии четверти экрана от верха.
			int FromTop = (ScrY - (Global->Opt->ViOpt.ShowKeyBar ? 2 : 1)) / 4;

			if (FromTop<0 || FromTop>ScrY)
				FromTop = 0;

			Up(FromTop, false);
		}

		AdjustSelPosition = true;
		Show();
		AdjustSelPosition = false;
	}
}


bool Viewer::GetWrapMode() const
{
	return m_Wrap;
}

void Viewer::SetWrapMode(bool Wrap)
{
	Viewer::m_Wrap=Wrap;
}

void Viewer::EnableHideCursor(int HideCursor)
{
	m_HideCursor=HideCursor;
}

bool Viewer::GetWrapType() const
{
	return m_WordWrap;
}

void Viewer::SetWrapType(bool TypeWrap)
{
	Viewer::m_WordWrap=TypeWrap;
}

void Viewer::SetTempViewName(const string& Name, bool DeleteFolder)
{
	if (!Name.empty())
		strTempViewName = ConvertNameToFull(Name);
	else
	{
		strTempViewName.clear();
		DeleteFolder = false;
	}

	m_DeleteFolder=DeleteFolder;
}

void Viewer::SetTitle(const string& Title)
{
	strTitle = Title;
}

void Viewer::SetFilePos(long long Pos)
{
	FilePos=Pos;
	AdjustFilePos();
};

void Viewer::SetPluginData(string_view const PluginData)
{
	strPluginData = PluginData;
}

void Viewer::SetNamesList(NamesList& List)
{
	ViewNamesList = std::move(List);
}

int Viewer::vread(wchar_t *Buf, int Count, wchar_t *Buf2)
{
	if (Count <= 0)
		return 0;

	size_t ReadSize = 0;

	if (IsUnicodeCodePage(m_Codepage))
	{
		Reader.Read(Buf, Count, &ReadSize);

		if (CP_REVERSEBOM == m_Codepage)
		{
			swap_bytes(Buf, Buf, ReadSize);
		}

		if (ReadSize & 1)
		{
			Buf[ReadSize / 2] = Utf::REPLACE_CHAR;
		}

		ReadSize /= 2;
	}
	else
	{
		char *TmpBuf = vread_buffer.data();
		char_ptr Buffer;
		if (static_cast<size_t>(Count) > vread_buffer.size())
		{
			Buffer.reset(Count);
			TmpBuf = Buffer.get();
		}
		Reader.Read(TmpBuf, Count, &ReadSize);
		const auto ConvertSize = ReadSize;

		if (m_Codepage == CP_UTF8)
		{
			int tail;
			ReadSize = Utf8::get_chars({ TmpBuf, ConvertSize }, Buf, Count, tail);

			if (Buf2)
			{
				std::copy_n(Buf, ReadSize, Buf2);
			}

			if (tail)
			{
				Reader.Unread(tail);
			}
		}
		else if (m_Codepage == MB.GetCP())
		{
			ReadSize = 0;
			for (size_t i = 0; i < ConvertSize; )
			{
				bool EndOfData = false;
				const auto clen = MB.GetChar({ TmpBuf + i, ConvertSize - i }, Buf[ReadSize], &EndOfData);
				if (clen)
				{
					if (Buf2)
						Buf2[ReadSize] = Buf[ReadSize];

					i += clen;
					++ReadSize;
				}
				else
				{
					if (EndOfData)
					{
						Reader.Unread(ConvertSize - i);
						break;
					}
					else // invalid sequence
					{
						if (Buf2)
							Buf2[ReadSize] = L'?';

						Buf[ReadSize++] = Utf::REPLACE_CHAR;
						++i;
					}
				}
			}
		}
		else
		{
			ReadSize = encoding::get_chars(m_Codepage, { TmpBuf, ConvertSize }, Buf, Count);
		}
	}

	return (int)ReadSize;
}

bool Viewer::vseek(long long Offset, int Whence)
{
	if (FILE_CURRENT == Whence)
	{
		if (vgetc_ready)
		{
			int tail = vgetc_cb - vgetc_ib;
			Offset += tail;
			vgetc_ready = false;
		}
		if (0 == Offset)
			return true;
	}

	vgetc_ready = false;
	return ViewFile.SetPointer(Offset, nullptr, Whence);
}

long long Viewer::vtell() const
{
	auto Ptr = ViewFile.GetPointer();

	if (vgetc_ready)
		Ptr -= (vgetc_cb - vgetc_ib);

	return Ptr;
}

bool Viewer::veof() const
{
	if (vgetc_ready && vgetc_ib < vgetc_cb)
		return false;
	else
		return ViewFile.Eof();
}

bool Viewer::vgetc(wchar_t *pCh)
{
	if (!vgetc_ready)
		vgetc_cb = vgetc_ib = (int)(vgetc_composite = 0);

	if (vgetc_cb - vgetc_ib < (pCh ? 4 : 1+4) && !ViewFile.Eof())
	{
		vgetc_cb -= vgetc_ib;
		if (vgetc_cb && vgetc_ib)
			std::copy_n(vgetc_buffer + vgetc_ib, vgetc_cb, vgetc_buffer);
		vgetc_ib = 0;

		size_t nr = 0;
		Reader.Read(vgetc_buffer + vgetc_cb, std::size(vgetc_buffer) - vgetc_cb, &nr);
		vgetc_cb += (int)nr;
	}

	vgetc_ready = true;

	if (vgetc_composite)
	{
		if (pCh)
			*pCh = vgetc_composite;
		vgetc_composite = 0;
		return true;
	}

	if (!pCh)
		return true;

	if (vgetc_cb <= vgetc_ib)
		return false;

	switch (m_Codepage)
	{
		case CP_REVERSEBOM:
			if (vgetc_ib == vgetc_cb-1)
				*pCh = Utf::REPLACE_CHAR;
			else
				*pCh = (wchar_t)((vgetc_buffer[vgetc_ib] << 8) | vgetc_buffer[vgetc_ib+1]);
			vgetc_ib += 2;
		break;
		case CP_UNICODE:
			if (vgetc_ib == vgetc_cb-1)
				*pCh = Utf::REPLACE_CHAR;
			else
				*pCh = (wchar_t)((vgetc_buffer[vgetc_ib+1] << 8) | vgetc_buffer[vgetc_ib]);
			vgetc_ib += 2;
		break;
		case CP_UTF8:
		{
			wchar_t w[2];
			const std::string_view View(vgetc_buffer, vgetc_cb);
			auto Iterator = View.cbegin() + vgetc_ib;
			const auto WideCharsNumber = Utf8::get_char(Iterator, View.cend(), w[0], w[1]);
			vgetc_ib = Iterator - View.cbegin();
			*pCh = w[0];
			if (WideCharsNumber > 1)
				vgetc_composite = w[1];
			break;
		}
		default:
			if (m_Codepage == MB.GetCP())
			{
				bool DataEnd = false;
				const auto clen = MB.GetChar({ vgetc_buffer + vgetc_ib, static_cast<size_t>(vgetc_cb - vgetc_ib) }, *pCh, &DataEnd);
				if (clen)
				{
					vgetc_ib += static_cast<int>(clen);
				}
				else
				{
					if (DataEnd)
					{
						*pCh = Utf::REPLACE_CHAR;
						vgetc_ib = vgetc_cb;
					}
					else // bad sequence
					{
						*pCh = Utf::REPLACE_CHAR;
						++vgetc_ib;
					}
				}
			}
			else
			{
				encoding::get_chars(m_Codepage, { vgetc_buffer + vgetc_ib, 1 }, pCh, 1);
				++vgetc_ib;
			}
		break;
	}

	return true;
}

wchar_t Viewer::vgetc_prev()
{
	const auto pos = vtell();
	if ( pos <= 0 )
		return L'\0';

	const auto CharSize = getCharSize();
	if ( pos < CharSize )
		return Utf::REPLACE_CHAR;

	size_t BytesToRead;

	if (CharSize < 0) // UTF-8 or MB decoder
	{
		BytesToRead = (CharSize == -1? 4 : -CharSize); // -1 -- UTF8
		BytesToRead = std::min(BytesToRead, static_cast<size_t>(pos));
	}
	else
	{
		BytesToRead = static_cast<size_t>(CharSize);
	}

	size_t BytesRead = 0;
	char RawBuffer[4] = {};
	if (vseek(-static_cast<int>(BytesToRead), FILE_CURRENT))
		Reader.Read(RawBuffer, BytesToRead, &BytesRead);

	vseek(pos, FILE_BEGIN);

	wchar_t Result = Utf::REPLACE_CHAR;
	if (BytesRead == BytesToRead)
	{
		switch (m_Codepage)
		{
		case CP_REVERSEBOM:
			Result = MAKEWORD(RawBuffer[1], RawBuffer[0]);
			break;

		case CP_UNICODE:
			Result = MAKEWORD(RawBuffer[0], RawBuffer[1]);
			break;

		case CP_UTF8:
			{
				int tail = 0;
				wchar_t CharBuffer[4];
				const auto Length = Utf8::get_chars({ RawBuffer, BytesRead }, CharBuffer, std::size(CharBuffer), tail);
				if (!tail && Length)
				{
					Result = CharBuffer[Length - 1];
				}
			}
			break;

		default:
			if (CharSize == 1)
			{
				encoding::get_chars(m_Codepage, { RawBuffer, 1 }, &Result, 1);
			}
			else
			{
				assert(MB.GetCP() == m_Codepage);
				for (size_t i = 0; i < BytesRead; ++i)
				{
					wchar_t Char;
					if (MB.GetChar({ RawBuffer + i, BytesRead - i }, Char) == BytesRead - i)
					{
						Result = Char;
						break;
					}
				}
			}
			break;
		}
	}
	return Result;
}

void Viewer::GoTo(bool ShowDlg, long long Offset, unsigned long long Flags)
{
	long long NewLeftPos = -1;

	int IsOffsetRelative = 0;

	if (ShowDlg)
	{
		if (!m_GotoHex)
		{
			m_GotoHex = m_DisplayMode == VMT_HEX;
		}

		goto_coord Row{};
		goto_coord Col{};
		if (!GoToRowCol(Row, Col, *m_GotoHex, L"ViewerGotoPos"sv))
			return;

		if (Row.exist)
		{
			Offset = Row.percent? FromPercent(Row.value, FileSize) : Row.value;
			IsOffsetRelative = Row.relative;
		}

		if (m_DisplayMode == VMT_TEXT && !m_Wrap && Col.exist)
		{
			NewLeftPos = Col.percent? 0 : Col.value;
			if (Col.relative)
				NewLeftPos = LeftPos + NewLeftPos * Row.relative;
		}
	}
	else
	{
		IsOffsetRelative = Flags & VSP_RELATIVE? Offset < 0? -1 : 1 : 0;

		if (Flags&VSP_PERCENT)
		{
			Offset = FromPercent(Offset, FileSize);
		}
	}

	FilePos = IsOffsetRelative? FilePos + Offset * IsOffsetRelative : Offset;
	FilePos = std::clamp(FilePos, 0ll, FileSize);

	AdjustFilePos();
	if (NewLeftPos != -1)
		LeftPos = NewLeftPos;

	if (!(Flags&VSP_NOREDRAW))
		Show();
}

void Viewer::AdjustFilePos()
{
	if (m_DisplayMode == VMT_TEXT)
	{
		wchar_t ch;
		FilePos -= FilePos % getChSize(m_Codepage);

		vseek(FilePos, FILE_BEGIN);
		if (m_Codepage != CP_UTF8)
		{
			vgetc(&ch);
		}
		else
		{
			vgetc(nullptr);
			if (vgetc_ib < vgetc_cb)
			{
				if (0x80 == (vgetc_buffer[vgetc_ib] & 0xC0))
				{
					if (++vgetc_ib < vgetc_cb)
					{
						if (0x80 == (vgetc_buffer[vgetc_ib] & 0xC0))
						{
							if (++vgetc_ib < vgetc_cb)
							{
								if (0x80 == (vgetc_buffer[vgetc_ib] & 0xC0))
									++vgetc_ib;
							}
						}
					}
				}
				else
				{
					vgetc(&ch);
				}
			}
		}

		FilePos = vtell();
		Up(1, true);
	}
}

void Viewer::SetFileSize()
{
	if (!ViewFile)
		return;

	unsigned long long uFileSize = 0; // BUGBUG, sign
	// BUGBUG check result
	(void)ViewFile.GetSize(uFileSize);
	FileSize=uFileSize;
}


void Viewer::GetSelectedParam(long long &Pos, long long &Length, DWORD &Flags) const
{
	Pos=SelectPos;
	Length=SelectSize;
	Flags=SelectFlags;
}

// Flags=0x01 - показывать [делать Show()]
//       0x02 - "обратный поиск" ?
//
void Viewer::SelectText(const long long &match_pos,const long long &search_len, const DWORD flags)
{
	if (!ViewFile)
		return;

	SelectPos = match_pos;
	SelectSize = search_len;
	SelectFlags = flags;
	if ( SelectSize < 0 )
		return;

	if (m_DisplayMode == VMT_TEXT)
	{
		FilePos = SelectPos;
		Up(1, true);
		LeftPos = 0;

		if ( !m_Wrap )
		{
			vseek(vString.nFilePos = FilePos, FILE_BEGIN);
			vString.Data.clear();
			ReadString(&vString, (int)(SelectPos-FilePos), false);

			if ( vString.eol_length == 0 )
			{
				int found_offset = static_cast<int>(vString.Data.size());
				if ( found_offset > Width-10 )
					LeftPos = (Width <= 10 ? found_offset : found_offset + 10 - Width);
			}
		}
	}
	else
	{
		const auto LineSize = GetModeDependentLineSize();
		FilePos = (FilePos % LineSize) + LineSize * (SelectPos / LineSize);
		FilePos = (FilePos < SelectPos? FilePos : (FilePos > LineSize? FilePos - LineSize : 0));
	}

	if (flags & 1)
	{
		AdjustSelPosition = true;
		Show();
		AdjustSelPosition = false;
	}
}


int Viewer::ViewerControl(int Command, intptr_t Param1, void *Param2)
{
	switch (Command)
	{
		case VCTL_GETINFO:
		{
			const auto Info=static_cast<ViewerInfo*>(Param2);
			if (CheckStructSize(Info))
			{
				memset(&Info->ViewerID,0,Info->StructSize-sizeof(Info->StructSize));
				Info->ViewerID = ViewerID;
				Info->WindowSizeX=ObjWidth();
				Info->WindowSizeY = m_Where.height();
				Info->FilePos=FilePos;
				Info->FileSize=FileSize;
				Info->CurMode.CodePage=m_Codepage;
				Info->CurMode.Flags=0;
				if (m_Wrap) Info->CurMode.Flags|=VMF_WRAP;
				if (m_WordWrap) Info->CurMode.Flags|=VMF_WORDWRAP;
				Info->CurMode.ViewMode = m_DisplayMode;
				Info->Options=0;

				if (Global->Opt->ViOpt.SavePos)
					Info->Options|=VOPT_SAVEFILEPOSITION;

				if (ViOpt.AutoDetectCodePage)
					Info->Options|=VOPT_AUTODETECTCODEPAGE;

				if (ViOpt.ShowScrollbar)
					Info->Options |= VOPT_SHOWSCROLLBAR;

				if (m_bQuickView)
					Info->Options |= VOPT_QUICKVIEW;

				Info->TabSize=ViOpt.TabSize;
				Info->LeftPos=LeftPos;
				return TRUE;
			}

			break;
		}
		/*
		   Param2 = ViewerSetPosition
		           сюда же будет записано новое смещение
		           В основном совпадает с переданным
		*/
		case VCTL_SETPOSITION:
		{
			const auto vsp = static_cast<ViewerSetPosition*>(Param2);
			if (CheckStructSize(vsp))
			{
				bool isReShow=vsp->StartPos != FilePos;

				if ((LeftPos=vsp->LeftPos) < 0)
					LeftPos=0;

				GoTo(false, vsp->StartPos, vsp->Flags);

				if (isReShow && !(vsp->Flags&VSP_NOREDRAW))
					Global->ScrBuf->Flush();

				if (!(vsp->Flags&VSP_NORETNEWPOS))
				{
					vsp->StartPos=FilePos;
					vsp->LeftPos=LeftPos;
				}

				return TRUE;
			}

			break;
		}
		// Param2=ViewerSelect
		case VCTL_SELECT:
		{
			const auto vs = static_cast<const ViewerSelect*>(Param2);
			if (CheckStructSize(vs))
			{
				long long SPos=vs->BlockStartPos;
				int SSize=vs->BlockLen;

				if (SPos < FileSize)
				{
					if (SPos+SSize > FileSize)
					{
						SSize=(int)(FileSize-SPos);
					}

					SelectText(SPos,SSize,0x1);
					Global->ScrBuf->Flush();
					return TRUE;
				}
			}
			else if (!Param2)
			{
				SelectSize = -1;
				Show();
			}

			break;
		}
		/* Функция установки Keybar Labels
		     Param2 = nullptr - восстановить, пред. значение
		     Param2 = -1   - обновить полосу (перерисовать)
		     Param2 = KeyBarTitles
		*/
		case VCTL_SETKEYBAR:
		{
			const auto Kbt = static_cast<const FarSetKeyBarTitles*>(Param2);

			if (!Kbt)
			{        // восстановить пред значение!
				if (HostFileViewer)
					HostFileViewer->InitKeyBar();
			}
			else
			{
				if ((intptr_t)Param2 != -1) // не только перерисовать?
				{
					if(CheckStructSize(Kbt))
						m_ViewKeyBar->Change(Kbt->Titles);
					else
						return FALSE;
				}
				m_ViewKeyBar->Show();
				Global->ScrBuf->Flush(); //?????
			}

			return TRUE;
		}
		// Param2=0
		case VCTL_REDRAW:
		{
			Global->WindowManager->RefreshWindow(GetOwner());
			Global->WindowManager->PluginCommit();
			Global->ScrBuf->Flush();
			return TRUE;
		}
		// Param2=0
		case VCTL_QUIT:
		{
			/* $ 28.12.2002 IS
			   Разрешаем выполнение VCTL_QUIT только для viewer-а, который
			   не является панелью информации и быстрого просмотра (т.е.
			   фактически панелей на экране не видно)
			*/
			if (!Global->WindowManager->IsPanelsActive())
			{
				/* $ 29.09.2002 IS
				   без этого не закрывался viewer, а просили именно это
				*/
				if (HostFileViewer)
				{
					Global->WindowManager->DeleteWindow(HostFileViewer->shared_from_this());
					HostFileViewer->SetExitCode(0);
					Global->WindowManager->PluginCommit();
				}

				return TRUE;
			}
		}
		[[fallthrough]];
		/* Функция установки режимов
		     Param2 = ViewerSetMode
		*/
		case VCTL_SETMODE:
		{
			const auto vsmode = static_cast<const ViewerSetMode*>(Param2);

			if (CheckStructSize(vsmode))
			{
				bool isRedraw = (vsmode->Flags&VSMFL_REDRAW) != 0;

				switch (vsmode->Type)
				{
					case VSMT_VIEWMODE:
						return ProcessDisplayMode(static_cast<VIEWER_MODE_TYPE>(vsmode->iParam), isRedraw);
					case VSMT_WRAP:
						ProcessWrapMode(vsmode->iParam,isRedraw);
						return TRUE;
					case VSMT_WORDWRAP:
						ProcessTypeWrapMode(vsmode->iParam,isRedraw);
						return TRUE;
				}
			}

			return FALSE;
		}
		case VCTL_GETFILENAME:
		{
			if (Param2&&(size_t)Param1>strFullFileName.size())
			{
				*std::copy(ALL_CONST_RANGE(strFullFileName), static_cast<wchar_t*>(Param2)) = L'\0';
			}

			return static_cast<int>(strFullFileName.size()+1);
		}
	}

	return FALSE;
}

bool Viewer::ProcessDisplayMode(VIEWER_MODE_TYPE newMode, bool isRedraw)
{
	switch (newMode)
	{
	case VMT_TEXT:
		m_DisplayMode = VMT_TEXT;
		break;

	case VMT_HEX:
		m_DisplayMode = VMT_HEX;
		break;

	case VMT_DUMP:
		m_DisplayMode = VMT_DUMP;
		break;

	default:
		return false;
	}

	AdjustFilePos();

	if (isRedraw)
	{
		ChangeViewKeyBar();
		Show();
	}

	return true;
}

int Viewer::ProcessWrapMode(int newMode, bool isRedraw)
{
	int oldWrap=m_Wrap;
	m_Wrap=newMode&1;

	if (m_Wrap)
		LeftPos = 0;

	AdjustFilePos();

	if (isRedraw)
	{
		ChangeViewKeyBar();
		Show();
	}

	Global->Opt->ViOpt.ViewerIsWrap = m_Wrap != 0;
	return oldWrap;
}

int Viewer::ProcessTypeWrapMode(int newMode, bool isRedraw)
{
	int oldTypeWrap=m_WordWrap;
	m_WordWrap=newMode&1;

	if (!m_Wrap)
	{
		m_Wrap=!m_Wrap;
		LeftPos = 0;
	}

	AdjustFilePos();

	if (isRedraw)
	{
		ChangeViewKeyBar();
		Show();
	}

	Global->Opt->ViOpt.ViewerWrap = m_WordWrap;
	return oldTypeWrap;
}

uintptr_t Viewer::GetDefaultCodePage()
{
	intptr_t cp = Global->Opt->ViOpt.DefaultCodePage;
	if (cp == CP_ACP)
		cp = encoding::codepage::ansi();
	else if (cp == CP_OEMCP)
		cp = encoding::codepage::oem();

	if (cp < 0 || !IsCodePageSupported(cp))
		cp = encoding::codepage::ansi();

	return cp;
}

void Viewer::ReadEvent()
{
	Global->CtrlObject->Plugins->ProcessViewerEvent(VE_READ,nullptr, this);
	bVE_READ_Sent = true;
}

void Viewer::CloseEvent()
{
	if (!OpenFailed && bVE_READ_Sent)
	{
		bVE_READ_Sent=false;
		Global->CtrlObject->Plugins->ProcessViewerEvent(VE_CLOSE,nullptr, this);
	}
}

void Viewer::OnDestroy()
{
	CloseEvent();
}
