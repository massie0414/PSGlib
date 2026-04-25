#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#include <string.h>

#define FALSE 0
#define TRUE 1

#define VGM_OLD_HEADERSIZE 64  // 'old' VGM header size
#define VGM_BIG_HEADERSIZE 256 // 'big' VGM header size
#define VGM_HEADER_LOOPPOINT 0x1C
#define VGM_HEADER_FRAMERATE 0x24
#define VGM_DATA_OFFSET 0x34

#define VGM_GGSTEREO 0x4F
#define VGM_PSGFOLLOWS 0x50
#define VGM_FMFOLLOWS 0x51
#define VGM_FRAMESKIP_NTSC 0x62
#define VGM_FRAMESKIP_PAL 0x63
#define VGM_SAMPLESKIP_7N 0x70 // 0x7n  skip n+1 samples
#define VGM_SAMPLESKIP 0x61
#define VGM_ENDOFDATA 0x66

#define MAX_WAIT 7 // fits in 3 bits only

#define PSG_ENDOFDATA 0x00
#define PSG_LOOPMARKER 0x01
#define PSG_WAIT 0x38

#define CHANNELS 4

unsigned int loop_offset;
unsigned int data_offset;
gzFile fIN;
FILE *fOUT;

// Start volume and frequencies to impossible values, to make
// sure initial commands are not skipped.
// 初期コマンドがスキップされないように、音量と周波数をありえない値に設定して開始する。
unsigned char volume[CHANNELS] = {0xFF, 0xFF, 0xFF, 0xFF};        // 各チャンネルのボリューム
unsigned short freq[CHANNELS] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // 各チャンネルの周波数
int volume_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};       // 音量が変わったか
int freq_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};         // 周波数が変わったか
int hi_freq_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};      //
int frame_started = TRUE;
int pause_started = FALSE;
int pause_len = 0;

// latch volume silent on channel 0
// チャンネル0の音量を無音としてラッチする
unsigned char lastlatch = 0x9F;

int active[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};
int is_sfx = FALSE;
int warn_32 = FALSE;

void decLoopOffset(int n)
{
  loop_offset -= n;
}

void incLoopOffset(void)
{
  loop_offset++;
}

int checkLoopOffset(void)
{
  // returns 1 when loop_offset becomes 0
  // loop_offset が 0 になったときに 1 を返す
  return (loop_offset == 0);
}

void init_frame(int initial_state)
{
  int i;
  for (i = 0; i < CHANNELS; i++)
  {
    volume_change[i] = FALSE;
    freq_change[i] = FALSE;
    hi_freq_change[i] = FALSE;
  }
  frame_started = initial_state;
}

void add_command(unsigned char c)
{
  int chn, typ;
  if (c & 0x80)
  {
    // it's a latch
    chn = (c & 0x60) >> 5;
    typ = (c & 0x10) >> 4;
    if (typ == 1)
    {
      if (volume[chn] != (c & 0x0F))
      {
        // see if we're really changing the volume or not
        volume[chn] = c & 0x0F;
        volume_change[chn] = TRUE;
      }
    }
    else
    {
      if (
          (chn == 3) || ((freq[chn] & 0x0F) != (c & 0x0F)))
      {
        // see if we're really changing the low part of the frequency or not (saving noise channel retrigs!)
        // 実際に周波数の下位部分を変更しているかどうかを確認する（ノイズチャンネルの再トリガーを抑えるため！）
        freq[chn] = (freq[chn] & 0xFFF0) | (c & 0x0F);
        freq_change[chn] = TRUE;
      }

      if (
          (chn == 3) && (is_sfx) && (active[3]) && (!active[2]) && ((c & 0x3) == 0x3) && (!warn_32))
      {
        // 警告：チャンネル3（ノイズチャンネル）がチャンネル2のトーンを使用しています。
        // おそらく、チャンネル2も含めて vgm2psg を実行する必要があります。
        printf("Warning: channel 3 (the noise channel) is using channel 2 tone. You probably need to run vgm2psg including channel 2 too.\n");
        warn_32 = TRUE;
      }
    }
  }
  else
  {
    // it's a data (not a latch)
    chn = (lastlatch & 0x60) >> 5;
    typ = (lastlatch & 0x10) >> 4;
    if (typ == 1)
    {
      if (volume[chn] != (c & 0x0F))
      {
        // see if we're really changing the volume or not
        volume[chn] = c & 0x0F;
        volume_change[chn] = TRUE;
      }
    }
    else
    {
      if ((c & 0x3F) != (freq[chn] >> 4))
      {
        // see if we're really changing the high part of the frequency or not
        // 周波数の上位部分を実際に変更しているかどうかを確認する
        freq[chn] = (freq[chn] & 0x000F) | ((c & 0x3F) << 4);
        hi_freq_change[chn] = TRUE;

        // to update the high part of the frequency we need to update the low part too, there's no other way
        // 周波数の上位部分を更新するには、下位部分も更新する必要があり、それ以外の方法はない
        freq_change[chn] = TRUE;
      }
    }
  }
}

void dump_frame(void)
{
  int i;
  unsigned char c;
  for (i = 0; i < CHANNELS - 1; i++)
  {
    if (freq_change[i])
    {
      c = (freq[i] & 0x0F) | (i << 5) | 0x80; // latch channel 0-2 freq
      fputc(c, fOUT);
      if (hi_freq_change[i])
      {
        // DATA byte needed?

        // make sure DATA bytes have 1 as 6th bit
        c = (freq[i] >> 4) | 0x40;
        fputc(c, fOUT);
      }
    }

    if (volume_change[i])
    {
      // latch channel 0-2 volume
      c = 0x90 | (i << 5) | (volume[i] & 0x0F);
      fputc(c, fOUT);
    }
  }

  if (freq_change[3])
  {
    // latch channel 3 (noise)
    c = (freq[i] & 0x07) | 0xE0;
    fputc(c, fOUT);
  }

  if (volume_change[3])
  {
    // latch channel 3 volume
    c = 0x90 | (i << 5) | (volume[3] & 0x0F);
    fputc(c, fOUT);
  }
}

void dump_pause(void)
{
  if (pause_len > 0)
  {
    while (pause_len > MAX_WAIT)
    {
      // write PSG_WAIT+7 to file
      fputc(PSG_WAIT + MAX_WAIT, fOUT);

      // skip MAX_WAIT+1
      pause_len -= MAX_WAIT + 1;
    }
    if (pause_len > 0)
    {
      // write PSG_WAIT+[0 to 7] to file, don't do it if 0
      fputc(PSG_WAIT + (pause_len - 1), fOUT);
    }
  }
}

void found_pause(void)
{
  if (frame_started)
  {
    dump_frame();
    init_frame(FALSE);
  }
  pause_started = TRUE;
}

void found_frame(void)
{
  if (pause_started)
  {
    dump_pause();
  }
  frame_started = TRUE;

  pause_started = FALSE;
  pause_len = 0;
}

void empty_data(void)
{
  if (pause_started)
  {
    dump_pause();
    pause_len = 0;
    pause_started = FALSE;
  }
  else if (frame_started)
  {
    dump_frame();
    init_frame(FALSE);
  }
}

void writeLoopMarker(void)
{
  empty_data();
  fputc(PSG_LOOPMARKER, fOUT);
}

//====================================================================
// メイン処理
//====================================================================
int main(int argc, char *argv[])
{
  unsigned int i;
  int c;
  int leave = 0;
  int fatal = 0;
  int ss;
  int fs;
  int latched_chn = 0;
  int first_byte = TRUE;
  unsigned int file_signature;
  unsigned int frame_rate;
  int sample_divider = 735; // NTSC (default)

  printf("*** sverx's VGM to PSG converter ***\n");

  if (argc > 4)
  {
    // 致命的エラー：指定されたパラメータが多すぎます。最大で3つまでしか指定できません。
    printf("Fatal: too many parameters specified. Three parameters at max are allowed.\n");
  }

  if (argc < 3)
  {
    // 致命的エラー：指定されたパラメータが少なすぎます。少なくとも2つのパラメータが必要です。
    printf("Fatal: too few parameters specified. At least two parameters are required.\n");
  }

  if ((argc < 3) || (argc > 4))
  {
    // 使用方法：vgm2psg 入力ファイル.VGM 出力ファイル.PSG [[0][1][2][3]]
    printf("Usage: vgm2psg inputfile.VGM outputfile.PSG [[0][1][2][3]]\n");
    // 【オプション】SFX（効果音）を変換する際、3番目のパラメータで有効にするチャンネルを指定します。例：
    printf(" [optional] when converting SFXs, the third parameter specifies which channel(s) should be active, examples:\n");
    // 0 は、その SFX がチャンネル0のみを使用していることを意味します。
    printf("   0 means the SFX is using channel 0 only\n");
    // 1 は、その SFX がチャンネル1のみを使用していることを意味します。
    printf("   1 means the SFX is using channel 1 only\n");
    // 2 は、その SFX がチャンネル2のみを使用していることを意味します。
    printf("   2 means the SFX is using channel 2 only\n");
    // 3 は、その SFX がチャンネル3（ノイズ）のみを使用していることを意味します。
    printf("   3 means the SFX is using channel 3 (noise) only\n");
    // 23 は、その SFX がチャンネル2とチャンネル3（ノイズ）の両方を使用していることを意味します。
    printf("  23 means the SFX is using both channel 2 and channel 3 (noise)\n");
    // 123 は、その SFX がチャンネル1、チャンネル2、およびチャンネル3（ノイズ）を使用していることを意味します。
    printf(" 123 means the SFX is using channels 1 and 2 and channel 3 (noise)\n");
    return (1);
  }

  if (argc == 4)
  {
    for (i = 0; i < CHANNELS; i++)
    {
      active[i] = FALSE;
    }

    for (i = 0; i < strlen(argv[3]); i++)
    {
      switch (argv[3][i])
      {
      case '0':
        active[0] = TRUE;
        break;
      case '1':
        active[1] = TRUE;
        break;
      case '2':
        active[2] = TRUE;
        break;
      case '3':
        active[3] = TRUE;
        break;
      default:
        // 致命的エラー：オプションの第3パラメータには、0〜3の数字のみを含めることができます。
        printf("Fatal: the optional third parameter can only contains digits 0 to 3\n");
        return (1);
      }
    }

    // if (!(active[0] && active[1] && active[2] && active[3]))
    if (active[0] == FALSE
      || active[1] == FALSE
      || active[2] == FALSE
      || active[3] == FALSE
    )
    {
      is_sfx = TRUE;
      printf("Info: SFX conversion on channel(s): %s%s%s%s\n", active[0] ? "0" : "_", active[1] ? "1" : "_", active[2] ? "2" : "_", active[3] ? "3" : "_");
    }
  }

  init_frame(TRUE);

  // ファイルオープン
  fIN = gzopen(argv[1], "rb");
  if (!fIN)
  {
    printf("Fatal: can't open input VGM file\n");
    return (1);
  }

  // 4バイト読み込み
  gzread(fIN, &file_signature, 4);
  if (file_signature != 0x206d6756)
  {
    // check for 'Vgm ' file signature
    // 致命的エラー：入力ファイルが有効なVGM/VGZファイルではないようです。
    printf("Fatal: input file doesn't seem a valid VGM/VGZ file\n");
    return (1);
  }

  // seek to FRAMERATE in the VGM header
  // 圧縮ファイル内の読み取り位置を移動
  gzseek(
    fIN,  // 対象のファイル（入力ファイル）
    VGM_HEADER_FRAMERATE, //  0x24バイト目の位置まで読み取り位置を移動
    SEEK_SET  // ファイルの先頭からの位置を基準にする
  );

  //===========================================
  // read frame_rate
  gzread(
    fIN, 
    &frame_rate, // 読み込んだデータを格納する場所（メモリ）
    4
  );

  if (frame_rate == 60)
  {
    printf("Info: NTSC (60Hz) VGM detected\n");
  }
  else if (frame_rate == 50)
  {
    printf("Info: PAL (50Hz) VGM detected\n");
    sample_divider = 882; // PAL!
  }
  else
  {
    printf("Warning: unknown frame rate, assuming NTSC (60Hz)\n");
  }
  // read frame_rate
  //===========================================


  //===========================================
  // seek to LOOPPOINT in the VGM header
  gzseek(
    fIN, 
    VGM_HEADER_LOOPPOINT, // 0x1Cバイト目の位置まで読み取り位置を移動
    SEEK_SET
  );

  // read loop_offset
  gzread(
    fIN,
    &loop_offset,
    4
  );
  // seek to LOOPPOINT in the VGM header
  //===========================================

  //===========================================
  // seek to DATAOFFSET in the VGM header
  gzseek(
    fIN, 
    VGM_DATA_OFFSET,  // 0x34バイト目の位置まで読み取り位置を移動
    SEEK_SET
  );

  // read data_offset
  gzread(
    fIN, 
    &data_offset, 
    4
  );
  // seek to DATAOFFSET in the VGM header
  //===========================================

  if (data_offset != 0)
  {
    // skip VGM header
    gzseek(
      fIN, 
      VGM_DATA_OFFSET + data_offset,  // 0x34 + data_offset
      SEEK_SET
    );
    data_offset = VGM_DATA_OFFSET + data_offset;
  }
  else
  {
    // skip 'old' VGM header
    gzseek(
      fIN, 
      VGM_OLD_HEADERSIZE, 
      SEEK_SET
    );

    // note: some VGMs can have zero in the data_offset field and have 256 bytes long header instead of 64, filled with zeroes. We do a quick check here.
    // 注：一部のVGMでは、data_offset フィールドが 0 の場合があり、その場合ヘッダは 64バイトではなく 256バイトで、残りはゼロで埋められています。ここではその簡易チェックを行います。
    c = gzgetc(fIN);
    if (c == 0)
    {
      // 警告：不正な形式のVGMです。できる限り処理を試みます。
      printf("Warning: malformed VGM, will try my best\n");

      // skip 'big' VGM header
      gzseek(
        fIN, 
        VGM_BIG_HEADERSIZE, // 256
        SEEK_SET
      );
      data_offset = VGM_BIG_HEADERSIZE;
    }
    else
    {
      // skip 'old' VGM header
      gzseek(
        fIN,
        VGM_OLD_HEADERSIZE, // 64
        SEEK_SET
      );
      data_offset = VGM_OLD_HEADERSIZE;
    }
  }

  if (loop_offset != 0)
  {
    printf("Info: loop point at 0x%08x\n", loop_offset);
    loop_offset = 
    loop_offset
     + VGM_HEADER_LOOPPOINT // 0x1C
      - data_offset;
  }
  else
  {
    printf("Info: no loop point defined\n");

    // make it negative so that won't happen
    loop_offset = -1;
  }

  fOUT = fopen(argv[2], "wb");
  if (!fOUT)
  {
    printf("Fatal: can't write to output PSG file\n");
    return (1);
  }

  while ((!leave) && (!gzeof(fIN)))
  {
    c = gzgetc(fIN);
    decLoopOffset(1);
    if (checkLoopOffset())
    {
      writeLoopMarker();
    }

    switch (c)
    {

    case VGM_GGSTEREO:
      // stereo data byte follows
      // BETA: this is simply DISCARDED atm
      c = gzgetc(fIN);
      printf("Warning: GameGear stereo info discarded\n");
      decLoopOffset(1);
      if (checkLoopOffset())
        writeLoopMarker();
      break;

    case VGM_FMFOLLOWS:
      // discard this!
      c = gzgetc(fIN);
      c = gzgetc(fIN);
      printf("Warning: FM chip write discarded\n");
      decLoopOffset(2);
      if (checkLoopOffset())
        writeLoopMarker();
      break;

    case VGM_PSGFOLLOWS:
      // PSG byte follows

      c = gzgetc(fIN);

      if (c & 0x80)
      {
        lastlatch = c;                 // latch value
        latched_chn = (c & 0x60) >> 5; // isolate chn number
      }
      else
      {
        c |= 0x40; // make sure DATA bytes have 1 as 6th bit
      }

      if ((!is_sfx) || (active[latched_chn]))
      {
        // output only if on an active channel

        found_frame();

        if ((first_byte) && ((c & 0x80) == 0))
        {
          add_command(lastlatch);
          printf("Warning: added missing latch command in frame start\n");
        }
        add_command(c);
        first_byte = FALSE;
      }

      decLoopOffset(1);
      if (checkLoopOffset())
      {
        writeLoopMarker();
      }
      break;

    case VGM_FRAMESKIP_NTSC:
    case VGM_FRAMESKIP_PAL:

      // frame skip, now count how many
      found_pause();

      fs = 1;
      do
      {
        c = gzgetc(fIN);
        if ((c == VGM_FRAMESKIP_NTSC) || (c == VGM_FRAMESKIP_PAL))
          fs++;
        decLoopOffset(1);
      } while ((fs < MAX_WAIT) && ((c == VGM_FRAMESKIP_NTSC) || (c == VGM_FRAMESKIP_PAL)) && (!checkLoopOffset()));

      if ((c != VGM_FRAMESKIP_NTSC) && (c != VGM_FRAMESKIP_PAL))
      {
        gzungetc(c, fIN);
        incLoopOffset();
      }
      else if (checkLoopOffset())
      {
        writeLoopMarker();
      }

      pause_len += fs;

      first_byte = TRUE;

      break;

    case VGM_SAMPLESKIP:
      // sample skip, now count how many

      found_pause();

      c = gzgetc(fIN);
      ss = c;
      c = gzgetc(fIN);
      ss += c * 256;

      // samples to frames
      fs = ss / sample_divider;

      if ((ss % sample_divider) != 0)
      {
        printf("Warning: pause length isn't perfectly frame sync'd\n");
        if ((ss % sample_divider) > (sample_divider / 2)) // round to closest int
          fs++;
      }

      pause_len += fs;

      decLoopOffset(2);
      if (checkLoopOffset())
      {
        writeLoopMarker();
      }

      first_byte = TRUE;

      break;

    case VGM_ENDOFDATA:
      // end of data
      leave = 1;
      decLoopOffset(1);
      if (checkLoopOffset())
      {
        writeLoopMarker();
      }
      empty_data();
      fputc(PSG_ENDOFDATA, fOUT);
      break;

    default:
      // Drop compact (1 to 16) sample skip command
      if ((c & 0xf0) == VGM_SAMPLESKIP_7N)
      {
        printf("Warning: pause length isn't perfectly frame sync'd\n");
        break;
      }

      printf("Fatal: found unknown char 0x%02x\n", c);
      leave = 1;
      fatal = 1;
      break;
    }
  }

  gzclose(fIN);
  fclose(fOUT);

  if (!fatal)
  {
    printf("Info: conversion complete\n");
    return (0);
  }
  else
  {
    return (1);
  }
}
