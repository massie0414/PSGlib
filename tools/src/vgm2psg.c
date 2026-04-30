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

// 録音レート（Hz）
const int NTSC = 735;
const int PAL = 882;

unsigned char volume[CHANNELS] = {0xFF, 0xFF, 0xFF, 0xFF};        // 各チャンネルのボリューム
unsigned short freq[CHANNELS] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // 各チャンネルの周波数
int volume_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};       // 音量が変わったか
int freq_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};         // 周波数が変わったか
int hi_freq_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};      //

unsigned char lastlatch = 0b1001'1111;

int active[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};

// 初期化
void init_frame()
{
  for (int i = 0; i < CHANNELS; i++)
  {
    volume_change[i] = FALSE;
    freq_change[i] = FALSE;
    hi_freq_change[i] = FALSE;
  }
}

void add_command(
  unsigned char input_data,
  int is_sfx,
  int *warn_32
)
{
  if (input_data & 0b1000'0000)
  {
    // ラッチデータ

    // チャンネル
    int chn = (input_data & 0b0110'0000) >> 5;
    int typ = (input_data & 0b0001'0000) >> 4;
    if (typ == 1)
    {
      if (volume[chn] != (input_data & 0b0000'1111))
      {
        // see if we're really changing the volume or not
        volume[chn] = input_data & 0b0000'1111;
        volume_change[chn] = TRUE;
      }
    }
    else
    {
      if ((chn == 3) || ((freq[chn] & 0b0000'1111) != (input_data & 0b0000'1111)))
      {
        // see if we're really changing the low part of the frequency or not (saving noise channel retrigs!)
        // 実際に周波数の下位部分を変更しているかどうかを確認する（ノイズチャンネルの再トリガーを抑えるため！）
        freq[chn] = (freq[chn] & 0b1111'1111'1111'0000) | (input_data & 0b0000'1111);
        freq_change[chn] = TRUE;
      }

      if ((chn == 3) && (is_sfx) && (active[3]) && (!active[2]) && ((input_data & 0b0000'0011) == 0b0000'0011) && (*warn_32==FALSE))
      {
        // 警告：チャンネル3（ノイズチャンネル）がチャンネル2のトーンを使用しています。
        // おそらく、チャンネル2も含めて vgm2psg を実行する必要があります。
        printf("Warning: channel 3 (the noise channel) is using channel 2 tone. You probably need to run vgm2psg including channel 2 too.\n");
        *warn_32 = TRUE;
      }
    }
  }
  else
  {
    int chn = (lastlatch & 0b0110'0000) >> 5;
    int typ = (lastlatch & 0b0001'0000) >> 4;
    if (typ == 1)
    {
      if (volume[chn] != (input_data & 0b0000'1111))
      {
        // see if we're really changing the volume or not
        volume[chn] = input_data & 0b0000'1111;
        volume_change[chn] = TRUE;
      }
    }
    else
    {
      if ((input_data & 0b0011'1111) != (freq[chn] >> 4))
      {
        // see if we're really changing the high part of the frequency or not
        // 周波数の上位部分を実際に変更しているかどうかを確認する
        freq[chn] = (freq[chn] & 0b0000'0000'0000'1111) | ((input_data & 0b0011'1111) << 4);
        hi_freq_change[chn] = TRUE;

        // to update the high part of the frequency we need to update the low part too, there's no other way
        // 周波数の上位部分を更新するには、下位部分も更新する必要があり、それ以外の方法はない
        freq_change[chn] = TRUE;
      }
    }
  }
}

// 1フレーム分の音データを出力する関数
void dump_frame(FILE* fOUT)
{
  for (int i = 0; i < CHANNELS - 1; i++)
  {
    if (freq_change[i])
    {
      // latch channel 0-2 freq
      unsigned char c = 0b1000'0000 | (i << 5) | (freq[i] & 0b0000'1111);
      
      fputc( 
        c, 
        fOUT
      );

      if (hi_freq_change[i])
      {
        // DATA byte needed?

        // make sure DATA bytes have 1 as 6th bit
        unsigned char c = 0b0100'0000 | (freq[i] >> 4);
        fputc(
          c, 
          fOUT
        );
      }
    }

    if (volume_change[i])
    {
      // latch channel 0-2 volume
      unsigned char c = 0b1001'0000 | (i << 5) | (volume[i] & 0b0000'1111);
      fputc(
        c, 
        fOUT
      );
    }
  }

  if (freq_change[3])
  {
    // latch channel 3 (noise)
    unsigned char c = 0b1110'0000 | (freq[3] & 0b0000'0111);
    fputc(
      c, 
      fOUT
    );
  }

  if (volume_change[3])
  {
    // latch channel 3 volume
    unsigned char c = 0b1111'0000 | (volume[3] & 0b0000'1111);
    fputc(
      c,
      fOUT
    );
  }
}

// ポーズデータの出力
void dump_pause(
  FILE* fOUT, 
  int *pause_len,
  int *pause_started
)
{
  if (*pause_len > 0)
  {
    while (*pause_len > MAX_WAIT)
    {
      // write PSG_WAIT+7 to file
      fputc(
        PSG_WAIT + MAX_WAIT,  // 0x38 + 7
        fOUT
      );

      // skip MAX_WAIT+1
      *pause_len -= MAX_WAIT + 1;
    }
    if (*pause_len > 0)
    {
      // write PSG_WAIT+[0 to 7] to file, don't do it if 0
      fputc(
        PSG_WAIT + (*pause_len - 1), // 0x38 + 
        fOUT
      );
    }
  }

  *pause_len = 0;
  *pause_started = FALSE;
}

void found_pause(
  FILE* fOUT,
  int* frame_started,
  int* pause_started
)
{
  if (*frame_started)
  {
    // 1フレーム分の音データを出力する関数
    dump_frame(fOUT);

    // 初期化
    init_frame();

    *frame_started = FALSE;
  }

  *pause_started = TRUE;
}

void empty_data(
  FILE* fOUT,
  int *pause_len,
  int *pause_started,
  int *frame_started
)
{
  if (pause_started)
  {
    // ポーズデータの出力
    dump_pause(
      fOUT, 
      pause_len,
      pause_started
    );
  }
  else if (*frame_started)
  {
    dump_frame(fOUT);
    init_frame();
    *frame_started = FALSE;
  }
}

// argcが4以外はメッセージを出力し、戻り値がTRUEになる
int checkArgc(int argc)
{
  int result = FALSE;

  if ((argc <= 2) || (argc >= 5))
  {
    if (argc >= 5)
    {
      // 致命的エラー：指定されたパラメータが多すぎます。最大で3つまでしか指定できません。
      printf("Fatal: too many parameters specified. Three parameters at max are allowed.\n");
    }

    if (argc <= 2)
    {
      // 致命的エラー：指定されたパラメータが少なすぎます。少なくとも2つのパラメータが必要です。
      printf("Fatal: too few parameters specified. At least two parameters are required.\n");
    }

    // 使用方法：vgm2psg 入力ファイル.VGM 出力ファイル.PSG [[0][1][2][3]]
    // 【オプション】SFX（効果音）を変換する際、3番目のパラメータで有効にするチャンネルを指定します。例：
    // 0 は、その SFX がチャンネル0のみを使用していることを意味します。
    // 1 は、その SFX がチャンネル1のみを使用していることを意味します。
    // 2 は、その SFX がチャンネル2のみを使用していることを意味します。
    // 3 は、その SFX がチャンネル3（ノイズ）のみを使用していることを意味します。
    // 23 は、その SFX がチャンネル2とチャンネル3（ノイズ）の両方を使用していることを意味します。
    // 123 は、その SFX がチャンネル1、チャンネル2、およびチャンネル3（ノイズ）を使用していることを意味します。

    printf("Usage: vgm2psg inputfile.VGM outputfile.PSG [[0][1][2][3]]\n");
    printf(" [optional] when converting SFXs, the third parameter specifies which channel(s) should be active, examples:\n");
    printf("   0 means the SFX is using channel 0 only\n");
    printf("   1 means the SFX is using channel 1 only\n");
    printf("   2 means the SFX is using channel 2 only\n");
    printf("   3 means the SFX is using channel 3 (noise) only\n");
    printf("  23 means the SFX is using both channel 2 and channel 3 (noise)\n");
    printf(" 123 means the SFX is using channels 1 and 2 and channel 3 (noise)\n");
    
    result = TRUE;
  }

  return result;
}

// sfxかどうか
int checkSFX(int argc, char *argv[])
{
  int is_sfx = FALSE;

  if (argc == 4)
  {
    for (unsigned int i = 0; i < strlen(argv[3]); i++)
    {
      printf("argv[3][%d]=%c\n", i, argv[3][i]);

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

    if (active[0] == FALSE
      || active[1] == FALSE
      || active[2] == FALSE
      || active[3] == FALSE
    )
    {
      // 1つでもFALSEならSFXとする
      is_sfx = TRUE;
      printf("Info: SFX conversion on channel(s): %s%s%s%s\n", active[0] ? "0" : "_", active[1] ? "1" : "_", active[2] ? "2" : "_", active[3] ? "3" : "_");
    }
  }

  return is_sfx;
}

int isVGM(gzFile fIN)
{
  int result = FALSE;

  unsigned int file_signature;

  // 4バイト読み込み
  gzread(
    fIN, 
    &file_signature,
    4
  );

  if (file_signature != 0x206d6756) // ファイルの先頭が "Vgm " という文字列かどうか
  {
    // check for 'Vgm ' file signature
    // 致命的エラー：入力ファイルが有効なVGM/VGZファイルではないようです。
    printf("Fatal: input file doesn't seem a valid VGM/VGZ file\n");
    result = TRUE;
  }
  
  return result;
}

// 録音レート（Hz）
int getSampleDivider(gzFile fIN)
{
  int sample_divider = NTSC; // NTSC (default)

  // seek to FRAMERATE in the VGM header
  // 圧縮ファイル内の読み取り位置を移動
  gzseek(
    fIN,  // 対象のファイル（入力ファイル）
    VGM_HEADER_FRAMERATE, //  0x24バイト目の位置まで読み取り位置を移動
    SEEK_SET  // ファイルの先頭からの位置を基準にする
  );

  unsigned int frame_rate;
  gzread(
    fIN, 
    &frame_rate, // 読み込んだデータを格納する場所（メモリ）
    4
  );

  if (frame_rate == 60)
  {
    printf("Info: NTSC (60Hz) VGM detected\n");
    sample_divider = NTSC; // NTSC
  }
  else if (frame_rate == 50)
  {
    printf("Info: PAL (50Hz) VGM detected\n");
    sample_divider = PAL; // PAL!
  }
  else
  {
    printf("Warning: unknown frame rate, assuming NTSC (60Hz)\n");
    sample_divider = NTSC; // NTSC
  }

  return sample_divider;
}

#if 0
int getLoopOffset(
  gzFile fIN,
  unsigned int data_offset
)
{
  int loop_offset = 0;

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

  return loop_offset;
}
#endif

unsigned int getDataOffset(gzFile fIN)
{
  unsigned int data_offset = 0;

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
    int input_data = gzgetc(fIN);
    if (input_data == 0)
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

  return data_offset;
}

void GGStereo(
  gzFile fIN, 
  FILE* fOUT,
  // int *loop_offset,
  int *pause_len,
  int *pause_started,
  int *frame_started
)
{
  printf("Warning: GameGear stereo info discarded\n");

  gzgetc(fIN);

  // *loop_offset -= 1;

  // if (*loop_offset == 0)
  // {
  //   empty_data(
  //     fOUT,
  //     pause_len,
  //     pause_started,
  //     frame_started
  //   );

  //   fputc(
  //     PSG_LOOPMARKER, // 0x01
  //     fOUT
  //   );
  // }
}

void fmFollows(
  gzFile fIN,
  FILE* fOUT, 
  // int *loop_offset,
  int *pause_len,
  int *pause_started,
  int *frame_started
)
{
  printf("Warning: FM chip write discarded\n");

  gzgetc(fIN);
  gzgetc(fIN);

  // *loop_offset -= 2;

  // if (*loop_offset == 0)
  // {
  //   empty_data(
  //     fOUT,
  //     pause_len,
  //     pause_started,
  //     frame_started
  //   );
  //   fputc(
  //     PSG_LOOPMARKER, // 0x01
  //     fOUT
  //   );
  // }
}

void psgFollows(
  gzFile fIN, 
  FILE* fOUT, 
  int is_sfx,
  int *first_byte,
  // int *loop_offset,
  int *pause_started,
  int *pause_len,
  int *frame_started,
  int *warn_32
)
{
  int input_data2 = gzgetc(fIN);
  int latched_chn = 0;

  if (input_data2 & 0b1000'0000)
  {
    // ラッチデータ
    lastlatch = input_data2;

    // isolate chn number
    latched_chn = (input_data2 & 0b0110'0000) >> 5; 
  }
  else
  {
    // ラッチデータではない

    // make sure DATA bytes have 1 as 6th bit
    input_data2 |= 0b0100'0000;
  }

  if (
    (!is_sfx)
      || (active[latched_chn])
  )
  {
    // アクティブなチャンネル上でのみ出力する
    if (pause_started)
    {
      // ポーズデータの出力
      dump_pause(
        fOUT, 
        pause_len,
        pause_started
      );
    }
    *frame_started = TRUE;

    if ((*first_byte) && ((input_data2 & 0b1000'0000) == 0))
    {
      add_command(
        lastlatch, 
        is_sfx,
        warn_32
      );

      printf("Warning: added missing latch command in frame start\n");
    }

    add_command(
      input_data2, 
      is_sfx,
      warn_32
    );

    *first_byte = FALSE;
  }

  // *loop_offset -= 1;
  // if (*loop_offset == 0)
  // {
  //   empty_data(
  //     fOUT,
  //     pause_len,
  //     pause_started,
  //     frame_started
  //   );

  //   fputc(
  //     PSG_LOOPMARKER, // 0x01
  //     fOUT
  //   );
  // }
}

void frameSkip(
  gzFile fIN,
  FILE* fOUT,
  int *first_byte,
  // int *loop_offset,
  int *pause_len,
  int *pause_started,
  int *frame_started
)
{
  found_pause(
    fOUT,
    frame_started,
    pause_started
  );

  int input_data2 = 0;

  int fs = 1;
  do
  {
    input_data2 = gzgetc(fIN);
    if (
        (input_data2 == VGM_FRAMESKIP_NTSC) // 0x62
    || (input_data2 == VGM_FRAMESKIP_PAL)  // 0x63
    )
    {
      fs++;
    }
    // *loop_offset -= 1;
  } 
  while (
    (fs < MAX_WAIT) // 7
    && (
      (input_data2 == VGM_FRAMESKIP_NTSC)   // 0x62
      || (input_data2 == VGM_FRAMESKIP_PAL) // 0x63
    )
    // && (*loop_offset != 0)
  );

  if (
    (input_data2 != VGM_FRAMESKIP_NTSC)   // 0x62
    && (input_data2 != VGM_FRAMESKIP_PAL) // 0x63
  )
  {
    gzungetc(input_data2, fIN); // 1バイト戻す
    // *loop_offset += 1;
  }
  // else if (*loop_offset == 0)
  // {
  //   empty_data(
  //     fOUT,
  //     pause_len,
  //     pause_started,
  //     frame_started
  //   );

  //   fputc(
  //     PSG_LOOPMARKER, // 0x01
  //     fOUT
  //   );
  // }

  *pause_len += fs;

  *first_byte = TRUE;
}

void sampleSkip(
  gzFile fIN,
  FILE* fOUT,
  int sample_divider,
  int* pause_len,
  // int* loop_offset,
  int* first_byte,
  int* pause_started,
  int* frame_started
)
{
  found_pause(
    fOUT, 
    frame_started,
    pause_started
  );

  int ss = gzgetc(fIN) + gzgetc(fIN) * 256;

  // samples to frames
  int fs = ss / sample_divider;

  if ((ss % sample_divider) != 0)
  {
    printf("Warning: pause length isn't perfectly frame sync'd\n");
    if ((ss % sample_divider) > (sample_divider / 2)) 
    {
      fs++;
    }
  }

  *pause_len += fs;

  // *loop_offset -= 2;

  // if (*loop_offset == 0)
  // {
  //   empty_data(
  //     fOUT,
  //     pause_len,
  //     pause_started,
  //     frame_started
  //   );

  //   fputc(
  //     PSG_LOOPMARKER, // 0x01
  //     fOUT
  //   );
  // }

  *first_byte = TRUE;
}

void endOfData(
  FILE* fOUT,
  int* pause_len,
  // int* loop_offset,
  int* pause_started,
  int* frame_started,
  int* leave
)
{
  // end of data
  *leave = TRUE;
  // *loop_offset -= 1;
  // if (*loop_offset == 0)
  // {
  //   empty_data(
  //     fOUT,
  //     pause_len,
  //     pause_started,
  //     frame_started
  //   );

  //   fputc(
  //     PSG_LOOPMARKER, // 0x01
  //     fOUT
  //   );
  // }

  empty_data(
    fOUT,
    pause_len,
    pause_started,
    frame_started
  );

  fputc(
    PSG_ENDOFDATA,  // 0x00
    fOUT
  );

}

//====================================================================
// メイン処理
//====================================================================
int main(int argc, char *argv[])
{
  printf("*** sverx's VGM to PSG converter ***\n");

  // argcのチェック
  if(checkArgc(argc))
  {
    // 引数の数が想定外の場合は終了
    return (1);
  }

  // SFXかどうか
  int is_sfx = checkSFX(argc, argv);

  // 入力ファイルのオープン
  gzFile fIN = gzopen(argv[1], "rb");
  if (!fIN)
  {
    printf("Fatal: can't open input VGM file\n");
    return (1);
  }

  // 出力ファイルのオープン
  FILE* fOUT = fopen(argv[2], "wb");
  if (!fOUT)
  {
    printf("Fatal: can't write to output PSG file\n");
    return (1);
  }

  // VGMファイルかどうかのチェック
  if (isVGM(fIN))
  {
    // VGMファイルではない
    return (1);
  }

  // 録音レートの取得
  int sample_divider = getSampleDivider(fIN);

  // データの位置を取得
  unsigned int data_offset = getDataOffset(fIN);

  // VGMファイルのループ開始位置（ループポイント）を取得
  // int loop_offset = getLoopOffset(fIN, data_offset);

  // seek
  gzseek(
    fIN, 
    data_offset,
    SEEK_SET
  );

  int leave = FALSE;  // ループを抜けるフラグ
  int fatal = FALSE;  // エラーがあったかどうかのフラグ
  int first_byte = TRUE;
  int pause_len = 0;  // 「待ち時間（ウェイト）の長さ」を蓄積するカウンタ
  int frame_started = TRUE;
  int pause_started = FALSE;
  int warn_32 = FALSE;

  while (
    (leave == FALSE) 
    && (!gzeof(fIN))  // ファイルが終わっているかどうか
  )
  {
    int input_data = gzgetc(fIN);

    // loop_offset -= 1;

    // printf("loop_offset=%d\n", loop_offset);

    // if (loop_offset == 0)
    // {
    //   empty_data(
    //     fOUT,
    //     &pause_len,
    //     &pause_started,
    //     &frame_started
    //   );

    //   fputc(
    //     PSG_LOOPMARKER, // 0x01
    //     fOUT
    //   );
    // }

    printf("input_data=%X\n", input_data);

    switch (input_data)
    {
      case VGM_GGSTEREO:  // 0x4F
      {
    printf("VGM_GGSTEREO\n");


        GGStereo(
          fIN,
          fOUT,
          // &loop_offset,
          &pause_len,
          &pause_started,
          &frame_started
        );
        break;
      }
      case VGM_FMFOLLOWS: // 0x51
      {
    printf("VGM_FMFOLLOWS\n");


        fmFollows(
          fIN, 
          fOUT, 
          // &loop_offset,
          &pause_len,
          &pause_started,
          &frame_started 
        );
        break;
      }
      case VGM_PSGFOLLOWS:  // 0x50
      {
    printf("VGM_PSGFOLLOWS\n");

        psgFollows(
          fIN, 
          fOUT, 
          is_sfx,
          &first_byte,
          // &loop_offset,
          &pause_started,
          &pause_len,
          &frame_started,
          &warn_32
        );
        break;
      }
      case VGM_FRAMESKIP_NTSC: // 0x62
      case VGM_FRAMESKIP_PAL:  // 0x63
      {
    printf("VGM_FRAMESKIP_NTSC\n");

        frameSkip(
          fIN,
          fOUT,
          &first_byte,
          // &loop_offset,
          &pause_len,
          &pause_started,
          &frame_started
        );
        break;
      }
      case VGM_SAMPLESKIP:  // 0x61
      {
    printf("VGM_SAMPLESKIP\n");

        // 待つ
        sampleSkip(
          fIN,
          fOUT,
          sample_divider,
          &pause_len,
          // &loop_offset,
          &first_byte,
          &pause_started,
          &frame_started
        );

        break;
      }
      case VGM_ENDOFDATA: // 0x66
      {
    printf("VGM_ENDOFDATA\n");

        endOfData(
          fOUT,
          &pause_len,
          // &loop_offset,
          &pause_started,
          &frame_started,
          &leave  // ループを抜けるためのフラグ
        );
        break;
      }
      default:
      {
    printf("default\n");

        // Drop compact (1 to 16) sample skip command
        if ((input_data & 0b1111'0000) == 0b0111'0000)  // 0x70
        {
          printf("Warning: pause length isn't perfectly frame sync'd\n");
          break;
        }

        printf("Fatal: found unknown char 0x%02x\n", input_data);
        leave = TRUE;
        fatal = TRUE;
        break;
      }
    }

    printf("leave=%d\n",leave);

  }

  //===================================
  // クローズ処理
  gzclose(fIN);
  fclose(fOUT);

  if (fatal == FALSE)
  {
    printf("Info: conversion complete\n");
    return (0);
  }
  else
  {
    return (1);
  }
  
}
