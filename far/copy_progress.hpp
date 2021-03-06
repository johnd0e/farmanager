﻿#ifndef COPY_PROGRESS_HPP_3D1EAAD8_8353_459C_8826_33AAAE06D01F
#define COPY_PROGRESS_HPP_3D1EAAD8_8353_459C_8826_33AAAE06D01F
#pragma once

/*
copy_progress.hpp
*/
/*
Copyright © 2016 Far Group
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

#include "taskbar.hpp"
#include "wakeful.hpp"
#include "datetime.hpp"
#include "plugin.hpp"

enum class lng;

class copy_progress: noncopyable
{
public:
	copy_progress(bool Move, bool Total, bool Time);

	bool IsCancelled() const { return m_IsCancelled; }
	bool IsTotalVisible() const { return m_Total; }

	// These functions shall not draw anything directly,
	// only update internal variables and call Flush().
	void SetNames(const string& Src, const string& Dst);
	void SetProgressValue(unsigned long long CompletedSize, unsigned long long TotalSize);
	void UpdateCurrentBytesInfo(unsigned long long NewValue);
	void UpdateAllBytesInfo(unsigned long long FileSize);

	// BUGBUG
	static string FormatCounter(lng CounterId, lng AnotherId, unsigned long long CurrentValue, unsigned long long TotalValue, bool ShowTotal, size_t MaxWidth);
	static size_t CanvasWidth();

private:
	bool CheckEsc();
	void Flush();
	void CreateBackground();
	void SetCurrentProgress(unsigned long long CompletedSize, unsigned long long TotalSize);
	void SetTotalProgress(unsigned long long CompletedSize, unsigned long long TotalSize);
	void UpdateTime(unsigned long long SizeDone, unsigned long long SizeToGo);
	unsigned long long GetBytesDone() const { return m_Bytes.Copied + m_Bytes.Skipped; }

	std::chrono::steady_clock::time_point m_CopyStartTime;
	IndeterminateTaskbar m_TB;
	wakeful m_Wakeful;
	SMALL_RECT m_Rect;

	string m_CurrentBar;
	size_t m_CurrentBarSize;
	int m_CurrentPercent;

	string m_TotalBar;
	size_t m_TotalBarSize;
	int m_TotalPercent;

	bool m_Move;
	bool m_Total;
	bool m_ShowTime;
	bool m_IsCancelled;
	FarColor m_Color;
	time_check m_TimeCheck;
	time_check m_SpeedUpdateCheck;
	string m_Src, m_Dst;
	string m_ScanName;
	string m_Time;
	string m_TimeLeft;
	string m_Speed;
	string m_FilesCopied;
	std::chrono::steady_clock::duration m_CalcTime;

	// BUGBUG
public:
	time_check m_SecurityTimeCheck;

	struct
	{
		size_t Copied;
		size_t Total;
	}
	m_Files;

	struct
	{
		unsigned long long Total;
		unsigned long long Copied;
		unsigned long long Skipped;
		unsigned long long CurrCopied;
	}
	m_Bytes;
};

#endif // COPY_PROGRESS_HPP_3D1EAAD8_8353_459C_8826_33AAAE06D01F
