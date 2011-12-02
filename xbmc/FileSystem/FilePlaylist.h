#ifndef FILE_PLAYLIST_H_
#define FILE_PLAYLIST_H_

#include "IFile.h"
#include "File.h"
#include "PlayList.h"
#include "utils/RingBuffer.h"
#include "utils/CriticalSection.h"
#include "utils/Thread.h"
#include "Key.h"

#include <vector>
#include <deque>

namespace XFILE
{

class CPlaylistData 
{
public:
  CPlaylistData(PLAYLIST::CPlayList* playList);
  virtual ~CPlaylistData();
  
  inline bool IsLoaded() const
  { 
    return m_playlistLastPos >= 0;
  }
  
  inline bool IsFinished() const
  {
    return !HasPendingSegments() && !m_playList->CanAdd();
  }
  
  inline bool HasPendingSegments() const
  {
    return m_playlistLastPos < m_playList->size() - 1;
  }
  
  inline bool IsEmpty() const
  {
    return m_playList->size() <= 0;
  }
  
  inline bool Load()
  { 
    if (IsFinished())
      return false;
    
    if (!m_playList->Load(m_playlistPath))
      return false;
  
    m_playlistLastPos = 0;
    
    if (Size())
    {
      m_targetDuration = atoi(FirstItem()->GetProperty("m3u8-targetDurationInSec").c_str());
      m_startDate = atoi(FirstItem()->GetProperty("m3u8-startDate").c_str());
    }
    
    return true;
  }
  
  inline bool SetPosition(unsigned int sequenceNo)
  {
    if (sequenceNo > LastSequenceNo())
      return false;
    
    m_playlistLastPos = 0;
    
    unsigned long currentSequenceNo = 0;
    while (m_playlistLastPos < Size())
    {
      currentSequenceNo = CurrentItem()->GetPropertyULong("m3u8-playlistSequenceNo");
      if (currentSequenceNo >= sequenceNo)
        return true;

      m_playlistLastPos++;
    }
    
    return false;
  }
  
  unsigned int FirstSequenceNo() const
  {
    return FirstItem() ? FirstItem()->GetPropertyULong("m3u8-playlistSequenceNo") : 0;
  }
  
  unsigned int LastSequenceNo() const
  {
    return LastItem() ? LastItem()->GetPropertyULong("m3u8-playlistSequenceNo") : 0;
  }
  
  unsigned int CurrentSequenceNo() const
  {
    return CurrentItem() ? CurrentItem()->GetPropertyULong("m3u8-playlistSequenceNo") : 0;    
  }
  
  CFileItemPtr FirstItem() const
  {
    return Size() ? (*m_playList)[0] : CFileItemPtr();
  }
  
  CFileItemPtr LastItem() const
  {
    return Size() ? (*m_playList)[Size() - 1] : CFileItemPtr();
  }
  
  CFileItemPtr CurrentItem() const
  {
    return m_playlistLastPos < Size() && m_playlistLastPos >= 0 ? (*m_playList)[m_playlistLastPos] :  CFileItemPtr();
  }
  
  int Size() const
  {
    return (int)m_playList->size();
  }
  
  int m_playlistBandwidth;
  int m_playlistLastPos;
  int m_targetDuration;
  int m_startDate;
  CStdString m_playlistPath;
  PLAYLIST::CPlayList* m_playList;
};
    
class BufferData
{
public:
  CRingBuffer *m_buffer;
  bool         m_bNeedResetDemuxer;
  int          m_nOriginPlaylist;
  int          m_nDuration;
  unsigned int m_nBufferTime;  
  unsigned int m_nSeq;

  BufferData();
  ~BufferData();
};
  
class CFilePlaylist : public IFile, public CThread
{
public:

  CFilePlaylist();
  virtual ~CFilePlaylist();

  virtual bool Open(const CURL& url);
  virtual bool Exists(const CURL& url);
  virtual int Stat(const CURL& url, struct __stat64* buffer);
  virtual unsigned int Read(void* lpBuf, int64_t uiBufSize);
  virtual int64_t Seek(int64_t iFilePosition, int iWhence = SEEK_SET);
  virtual void Close();
  virtual int64_t GetPosition();
  virtual int64_t GetLength();

  virtual CStdString GetContent();

  bool OnAction(const CAction &action);

  const CStdString& GetFingerprintBase64();
  const CStdString& GetKeyServerParams();

  bool SeekToTime(int nSecs);
  
  unsigned int GetStartTime();   // in seconds
  unsigned int GetCurrentTime(); // in seconds
  unsigned int GetTotalTime();   // in seconds
  
  bool IsEOF();
  void SetReadAheadBuffers(int nBuffers);
  bool IncQuality();
  bool DecQuality();
  
  //
  // load exactly 1 segment to the queue (will calculate required segment according to the last seq)
  //
  void ReadAhead();
  
  //
  // make sure the playlist is loaded and tuned on the right index
  // 
  bool ValidatePlaylist(CPlaylistData *pl);

protected:
  virtual void Process(); 
  bool ParsePath(const CStdString& strPath);
  void ClearBuffers();
  void ResetDemuxer();
  void SetPlayerTime();
  void NextBuffer();
  
  PLAYLIST::CPlayList* BuildPlaylist(const CStdString& playlistPath, bool appendToPlaylist = false);
  unsigned int ReadData(void* lpBuf, int64_t uiBufSize);
  bool BuildPlaylistByBandwidth(PLAYLIST::CPlayList* playListOfPlaylists);
  bool InsertPlaylistDataToList(CFileItemPtr item, PLAYLIST::CPlayList* playlist);
  bool BuildFilesToReadList(CFileItemList& fileToReadList, int& newCurrFileIndex, bool& isEndOfPlaylist);
  bool GetEncryptKey(const CStdString& encryptKeyUri, CStdString& encryptKeyValue);

  std::vector<CPlaylistData*> m_playlists;
  int                         m_nCurrPlaylist;
  unsigned int                m_nLastLoadedSeq;
  unsigned int                m_nStartTime;
  int                         m_nReadAheadBuffers;
  int                         m_nLastBufferSwitch;
  
  unsigned int m_lastReportedTime;
  
  CStdString m_playlistFilePath;
  CStdString m_fingerprintBase64;
  CStdString m_keyServerParams;
  CStdString m_bxOurl;
  CStdString m_trackingUrl;
  CStdString m_inningsIndex;
  CStdString m_quality;
  bool       m_isLive;
  bool       m_eof;
  bool       m_autoChooseQuality;
  int        m_startTime;       // where to start - in seconds
  int        m_prerollDuration; // where the actual stream starts (begining of the playlist can be just junk or preroll slide)
  CStdString m_preRollStrDuration;
  
  CStdString m_encryptKeyUri;
  CStdString m_encryptKeyValue;

  std::deque<BufferData*> m_buffersQueue;
  CRingBuffer* m_inProgressBuffer;
  CCriticalSection m_lock;
};

}
#endif /*FILE_PLAYLIST_H_*/
