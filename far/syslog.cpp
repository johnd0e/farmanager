/*
syslog.cpp

���⥬�� �⫠���� ��� :-)

*/

/* Revision: 1.03 28.04.2001 $ */

/*
Modify:
  28.04.2001 SVS
    - ����୮ ���⠢��� 䫠�, ����� ��� CONTEXT_INTEGER ��﫮
      CONTEXT_SEGMENTS
    ! ������ ��।���� �㭪権 ᥬ���⢠ SysLog()
    + �㭪�� ���� ����� ����� SysLogDump()
      �᫨ 㪠��⥫� �� 䠩� = NULL, � �㤥� ������� � �⠭����� ���,
      ���� � 㪠����� ������ 䠩�.
  26.01.2001 SVS
    ! DumpExeptionInfo -> DumpExceptionInfo ;-)
  23.01.2001 SVS
    + DumpExeptionInfo()
  22.12.2000 SVS
    + �뤥����� � ����⢥ ᠬ����⥫쭮�� �����
*/

#include "headers.hpp"
#pragma hdrstop
#include "internalheaders.hpp"

#if defined(SYSLOG)
char         LogFileName[MAX_FILE];
static FILE *LogStream=0;
static int   Indent=0;
static char *PrintTime(char *timebuf);
#endif

FILE * OpenLogStream(char *file)
{
#if defined(SYSLOG)
    time_t t;
    struct tm *time_now;
    char RealLogName[MAX_FILE];
//    char rfile[MAX_FILE];

    time(&t);
    time_now=localtime(&t);

    strftime(RealLogName,MAX_FILE,file,time_now);
    return _fsopen(RealLogName,"a+t",SH_DENYWR);
#else
    return NULL;
#endif
}

void OpenSysLog()
{
#if defined(SYSLOG)
    if ( LogStream )
        fclose(LogStream);

    GetModuleFileName(NULL,LogFileName,sizeof(LogFileName));
    strcpy(strrchr(LogFileName,'\\')+1,"far.log");
    LogStream=OpenLogStream(LogFileName);
    //if ( !LogStream )
    //{
    //    fprintf(stderr,"Can't open log file '%s'\n",LogFileName);
    //}
#endif
}

void CloseSysLog(void)
{
#if defined(SYSLOG)
    fclose(LogStream);
    LogStream=0;
#endif
}

void SysLog(int i)
{
#if defined(SYSLOG)
    Indent+=i;
    if ( Indent<0 )
        Indent=0;
#endif
}

#if defined(SYSLOG)
static char *PrintTime(char *timebuf)
{
  SYSTEMTIME st;
  GetLocalTime(&st);
  sprintf(timebuf,"%02d.%02d.%04d %2d:%02d:%02d.%04d",
      st.wDay,st.wMonth,st.wYear,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds);
  return timebuf;
}
#endif

void SysLog(char *fmt,...)
{
#if defined(SYSLOG)
  char msg[MAX_LOG_LINE];
  char timebuf[64];

  va_list argptr;
  va_start( argptr, fmt );

  vsprintf( msg, fmt, argptr );
  va_end(argptr);

  OpenSysLog();
  if ( LogStream )
  {
    fprintf(LogStream,"%s %*s %s\n",PrintTime(timebuf),Indent,"",msg);
    fflush(LogStream);
  }
  CloseSysLog();
#endif
}

void SysLogDump(char *Title,DWORD StartAddress,LPBYTE Buf,int SizeBuf,FILE *fp)
{
#if defined(SYSLOG)
  int CY=(SizeBuf+15)/16;
  int X,Y;
  int InternalLog=fp==NULL?TRUE:FALSE;

  char msg[MAX_LOG_LINE];
  char timebuf[64];

  if(InternalLog)
  {
    OpenSysLog();
    fp=LogStream;
    if(fp)
      fprintf(fp,"%s %*s (%s)\n",PrintTime(timebuf),Indent,"",NullToEmpty(Title));
  }

  if (fp)
  {
    char TmpBuf[17];
    int I;
    TmpBuf[16]=0;
    if(!InternalLog && Title && *Title)
      fprintf(fp,"%s\n",Title);
    for(Y=0; Y < CY; ++Y)
    {
      memset(TmpBuf,' ',16);
      fprintf(fp, " %08X: ",StartAddress+Y*16);
      for(X=0; X < 16; ++X)
      {
        if((I=Y*16+X < SizeBuf) != 0)
          fprintf(fp,"%02X ",Buf[Y*16+X]&0xFF);
        else
          fprintf(fp,"   ");
        TmpBuf[X]=I?(Buf[Y*16+X] < 32?'.':Buf[Y*16+X]):' ';
        if(X == 7)
          fprintf(fp, " ");
      }
      fprintf(fp,"| %s\n",TmpBuf);
    }
    fprintf(fp,"\n");
    fflush(fp);
  }

  if(InternalLog)
    CloseSysLog();
#endif
}

/*
������� 䠩�� "farexcpt.dmp"

00000: CountRecords:DWORD
Record {
    SizeRecord:DWORD    - ��騩 ࠧ���
    RecFlags:DWORD      - �������⥫�� 䫠�� (���� =0)
    Time:SYSTEMTIME     - The system time is expressed in Coordinated Universal Time (UTC))
    WinVer:OSVERSIONINFO
    SizePlugin:DWORD    - ࠧ��� ���� Plugin, �᫨ = 0 - � ���� Plugin
    {Plugin:PLUGINITEM} * SizePlugin
    Regs:CONTEXT
    ExcRecCount:DWORD   - ������⢮ �᪫�祭��, �᫨ = 0 - � ���� ExcRec
    {ExcRec[0]:EXCEPTION_RECORD
      SizeExcAddr:DWORD   - ࠧ��� ���� ExcAddr, �᫨ = 0 - � ���� ExcAddr
      {ExcAddr}
    } * ExcRecCount
    SizeStack:DWORD     - ࠧ��� ���� Stack, �᫨ = 0 - � ���� Stack
    {Stack} * SizeStack
}
*/
int DumpExceptionInfo(EXCEPTION_POINTERS *xp,struct PluginItem *Module)
{
  if(xp)
  {
    static char DumpExcptFileName[MAX_FILE];
    GetModuleFileName(NULL,DumpExcptFileName,sizeof(DumpExcptFileName));
    strcpy(strrchr(DumpExcptFileName,'\\')+1,"farexcpt.dmp");
    HANDLE fp=CreateFile(DumpExcptFileName,
                 GENERIC_READ|GENERIC_WRITE,
                 FILE_SHARE_READ|FILE_SHARE_WRITE,
                 NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_ARCHIVE,NULL);
    if(fp != INVALID_HANDLE_VALUE)
    {
      int NumExt;
      DWORD CountDump;
      DWORD SizeOfRecord;
      DWORD Pos,Temp;
      DWORD RecFlags=0;
      DWORD Zero=0;
      DWORD SizePlugin=0;

      // ������� ����⠥�
      CONTEXT *cn=xp->ContextRecord;
      struct _EXCEPTION_RECORD *xpn=xp->ExceptionRecord;
      // ����⠥� ࠧ��� �����
      SizeOfRecord=sizeof(SYSTEMTIME)+
                   sizeof(DWORD)+
                   sizeof(CONTEXT)+
                   sizeof(DWORD)+
                   sizeof(DWORD)+
                   sizeof(DWORD)+
                   sizeof(OSVERSIONINFO);
      if(Module && Module->ModuleName[0])
      {
        SizeOfRecord+=(SizePlugin=sizeof(struct PluginItem));
      }
      NumExt=0;
      while(xpn)
      {
        SizeOfRecord+=sizeof(EXCEPTION_RECORD)-
                      (EXCEPTION_MAXIMUM_PARAMETERS*sizeof(DWORD))+
                      sizeof(DWORD)*xpn->NumberParameters;
        if(xpn->ExceptionAddress)
          SizeOfRecord+=sizeof(DWORD); // ��⥬ ���� SizeExcAddr
        NumExt++;
        xpn=xpn->ExceptionRecord;
      }

      // �����⮢�� ���������
      if((Pos=GetFileSize(fp,NULL)) == -1 || !Pos)
        CountDump=0;
      else
      {
        SetFilePointer(fp,0,NULL,FILE_BEGIN);
        ReadFile(fp,&CountDump,sizeof(CountDump),&Temp,NULL);
      }
      CountDump++;
      SetFilePointer(fp,0,NULL,FILE_BEGIN);
      WriteFile(fp,&CountDump,sizeof(CountDump),&Temp,NULL);

      // ���� ���� ���襬 �६�
      // The system time is expressed in Coordinated Universal Time (UTC).
      SYSTEMTIME st;
      GetSystemTime(&st);
      SetFilePointer(fp,0,NULL,FILE_END);
      // ࠧ��� �����
      WriteFile(fp,&SizeOfRecord,sizeof(SizeOfRecord),&Temp,NULL);
      // 䫠��
      WriteFile(fp,&RecFlags,sizeof(RecFlags),&Temp,NULL);
      // �६�
      WriteFile(fp,&st,sizeof(st),&Temp,NULL);
      // OS
      WriteFile(fp,&WinVer,sizeof(WinVer),&Temp,NULL);

      // ��襬 ����� �� �������, �᫨ ����
      WriteFile(fp,&SizePlugin,sizeof(SizePlugin),&Temp,NULL);
      if(SizePlugin)
      {
        WriteFile(fp,&Module,sizeof(*Module),&Temp,NULL);
      }

      // ��襬 ॣ�����
      WriteFile(fp,cn,sizeof(CONTEXT),&Temp,NULL);

      // ������⢮ ������ �᪫�祭��
      WriteFile(fp,&NumExt,sizeof(NumExt),&Temp,NULL);

      // ���� �᪫�祭��
      xpn=xp->ExceptionRecord;
      while(xpn)
      {
        WriteFile(fp,&xpn->ExceptionCode,sizeof(DWORD),&Temp,NULL);
        WriteFile(fp,&xpn->ExceptionFlags,sizeof(DWORD),&Temp,NULL);
        Pos=0;//xpn->ExceptionRecord - ���� �⠢�� 0
        WriteFile(fp,&Pos,sizeof(DWORD),&Temp,NULL);
        WriteFile(fp,xpn->ExceptionAddress,sizeof(PVOID),&Temp,NULL);
        WriteFile(fp,&xpn->NumberParameters,sizeof(DWORD),&Temp,NULL);
        WriteFile(fp,xpn->ExceptionInformation,sizeof(DWORD)*xpn->NumberParameters,&Temp,NULL);

        // ����� �� ���� �᪫�祭��
        /* ����� ���� �� ���� ��� ������� :-( */
        //if(xpn->ExceptionAddress)
          Zero=0; // ���� 0
        WriteFile(fp,&Zero,sizeof(Zero),&Temp,NULL);
        // ... ����� ����
        //if(xpn->ExceptionAddress)
        xpn=xpn->ExceptionRecord;
      }

      // ����� �� �⥪� ������
      /* ����� ���� �� ���� ��� ������� :-( */
      Zero=0; // ���� 0
      WriteFile(fp,&Zero,sizeof(Zero),&Temp,NULL);
      // ... ����� ����

      CloseHandle(fp);
    }
  }
  return EXCEPTION_EXECUTE_HANDLER;
}
