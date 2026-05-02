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

enum Type
{
  FREQUENCY = 0,
  VOLUME = 1
};

#define PSG_LATCH_MASK            0b1000'0000
#define PSG_CH_MASK               0b0110'0000
#define PSG_HI_CH_MASK            0b0100'0000
#define PSG_TYPE_MASK             0b0001'0000
#define PSG_DATA_MASK             0b0000'1111
#define PSG_DATA_MASK2            0b0011'1111
#define PSG_FREQ_MASK             0b1111'1111'1111'0000
#define PSG_FREQ_MASK2            0b0000'0000'0000'1111
#define PSG_NOISE_FREQ_MASK       0b1110'0000 
#define PSG_NOISE_FREQ_TYPE_MASK  0b0000'0111  // ノイズ種類＋周波数
#define PSG_NOISE_FREQ_DATA_MASK  0b0000'0011  // 周波数
#define PSG_VOLUME_MASK           0b1001'0000
#define PSG_NOISE_VOLUME_MASK     0b1111'0000 

#define NOISE_CH 3

// unsigned char volume[CHANNELS] = {0xFF, 0xFF, 0xFF, 0xFF};        // 各チャンネルのボリューム
// unsigned short freq[CHANNELS] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // 各チャンネルの周波数
// int volume_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};       // 音量が変わったか
// int freq_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};         // 周波数が変わったか
// int hi_freq_change[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};      //
// int active[CHANNELS] = {FALSE, FALSE, FALSE, FALSE};

typedef struct {
  unsigned char volume[CHANNELS];
  unsigned short freq[CHANNELS];
  int volume_change[CHANNELS];
  int freq_change[CHANNELS];
  int hi_freq_change[CHANNELS];
  int active[CHANNELS];
} PSGState;

typedef struct {
  // 状態
  int pause_len;
  int pause_started;
  int frame_started;
  int first_byte;
  int warn_32;
  int latched_chn;
  unsigned char lastlatch;

  int is_sfx;
  int sample_divider;

  PSGState st;
} Context;

// 初期化
void init_frame(
  PSGState* st
)
{
  for (int i = 0; i < CHANNELS; i++)
  {
    st->volume[i] = 0xFF;
    st->freq[i] = 0xFFFF;
    st->volume_change[i] = FALSE;
    st->freq_change[i] = FALSE;
    st->hi_freq_change[i] = FALSE;
    st->active[i] = FALSE;
  }
}

// 1：ラッチデータ
int is_latch(int input_data)
{
  // return input_data & 0b1000'0000;
  return input_data & PSG_LATCH_MASK;
}

// チャンネル番号：0～3
int getChannel(int input_data)
{
  // return (input_data & 0b0110'0000) >> 5;
  return (input_data & PSG_CH_MASK) >> 5;
}

// 0:周波数 1:音量
int getType(int input_data)
{
  // return (input_data & 0b0001'0000) >> 4;
  return (input_data & PSG_TYPE_MASK) >> 4;
}

// データの抽出
int getData(int input_data)
{
  return (input_data & PSG_DATA_MASK);
}

// データの抽出（２バイト目）
int getData2(int input_data)
{
  return (input_data & PSG_DATA_MASK2);
}

// 周波数の抽出
unsigned short getFreq(unsigned short input_data)
{
  return (input_data & PSG_FREQ_MASK);
}

unsigned short getFreq2(unsigned short input_data)
{
  return (input_data & PSG_FREQ_MASK2);
}

void add_command(
  unsigned char input_data,
  // int is_sfx,
  // int *warn_32,
  // unsigned char lastlatch,
  // PSGState* st
  Context *ctx
)
{
  // int chn = (input_data & 0b0110'0000) >> 5;
  int chn = getChannel(input_data);
  int typ = getType(input_data);

  // if (input_data & 0b1000'0000)
  if (is_latch(input_data))
  {
    // ラッチデータ

    // int typ = (input_data & 0b0001'0000) >> 4;
    switch (typ)
    {
      case FREQUENCY:
      {
        // 周波数
        if (
          // (chn == 3) // ノイズ
          (chn == NOISE_CH) // ノイズ
          || 
          (
            // (st->freq[chn] & 0b0000'1111) != (input_data & 0b0000'1111)
            // (st->freq[chn] & PSG_DATA_MASK) != (input_data & PSG_DATA_MASK)
            getData(ctx->st.freq[chn]) != getData(input_data)
          )
        )
        {
          // see if we're really changing the low part of the frequency or not (saving noise channel retrigs!)
          // 実際に周波数の下位部分を変更しているかどうかを確認する（ノイズチャンネルの再トリガーを抑えるため！）
          // st->freq[chn] = (st->freq[chn] & 0b1111'1111'1111'0000) | (input_data & 0b0000'1111);
          // st->freq[chn] = (st->freq[chn] & PSG_FREQ_MASK) | (input_data & PSG_DATA_MASK);
          ctx->st.freq[chn] = getFreq(ctx->st.freq[chn]) | getData(input_data);
          ctx->st.freq_change[chn] = TRUE;
        }

        if (
          // (chn == 3) 
          (chn == NOISE_CH) 
          && (ctx->is_sfx) 
          && (ctx->st.active[NOISE_CH]) 
          && (!ctx->st.active[2]) 
          // && ((input_data & 0b0000'0011) == 0b0000'0011) 
          && ((input_data & PSG_NOISE_FREQ_DATA_MASK) == PSG_NOISE_FREQ_DATA_MASK) 
          && (ctx->warn_32 == FALSE)
        )
        {
          // 警告：チャンネル3（ノイズチャンネル）がチャンネル2のトーンを使用しています。
          // おそらく、チャンネル2も含めて vgm2psg を実行する必要があります。
          printf("Warning: channel 3 (the noise channel) is using channel 2 tone. You probably need to run vgm2psg including channel 2 too.\n");
          ctx->warn_32 = TRUE;
        }

        break;
      }
      case VOLUME:
      {
        // ボリューム
        // if (st->volume[chn] != (input_data & 0b0000'1111))
        // if (st->volume[chn] != (input_data & PSG_DATA_MASK))
        if (ctx->st.volume[chn] != getData(input_data))
        {
          // see if we're really changing the volume or not
          // st->volume[chn] = input_data & 0b0000'1111;
          // st->volume[chn] = input_data & PSG_DATA_MASK;
          ctx->st.volume[chn] = getData(input_data);
          ctx->st.volume_change[chn] = TRUE;
        }

        break;
      }
    }
  }
  else
  {
    // ラッチデータではない

    // int chn = (lastlatch & 0b0110'0000) >> 5;
    // int typ = (lastlatch & 0b0001'0000) >> 4;
    // if (typ == 1)
    switch(typ)
    {
      case FREQUENCY:
      {
        // 周波数
        // if ((input_data & 0b0011'1111) != (st->freq[chn] >> 4))
        // if ((input_data & PSG_DATA_MASK2) != (st->freq[chn] >> 4))
        if (getData(input_data) != (ctx->st.freq[chn] >> 4))
        {
          // 周波数の上位部分を実際に変更しているかどうかを確認する
          // st->freq[chn] = (st->freq[chn] & 0b0000'0000'0000'1111) | ((input_data & 0b0011'1111) << 4);
          // st->freq[chn] = (st->freq[chn] & PSG_FREQ_MASK2) | ((input_data & PSG_DATA_MASK2) << 4);
          ctx->st.freq[chn] = getFreq2(ctx->st.freq[chn]) | (getData2(input_data) << 4);
          ctx->st.hi_freq_change[chn] = TRUE;

          // 周波数の上位部分を更新するには、下位部分も更新する必要があり、それ以外の方法はない
          ctx->st.freq_change[chn] = TRUE;
        }
        break;
      }
      case VOLUME:
      {
        // 音量
        // if (st->volume[chn] != (input_data & 0b0000'1111))
        // if (st->volume[chn] != (input_data & PSG_DATA_MASK))
        if (ctx->st.volume[chn] != getData(input_data))
        {
          // see if we're really changing the volume or not
          // st->volume[chn] = input_data & 0b0000'1111;
          // st->volume[chn] = input_data & PSG_DATA_MASK;
          ctx->st.volume[chn] = getData(input_data);
          ctx->st.volume_change[chn] = TRUE;
        }
        break;
      }
    }
  }
}

// 1フレーム分の音データを出力する関数
void dump_frame(
  FILE* fOUT,
  PSGState *st
  // Context *ctx
)
{
  for (int i = 0; i < CHANNELS - 1; i++)
  {
    if (st->freq_change[i])
    {
      // latch channel 0-2 freq
      unsigned char c =
      //  0b1000'0000 
       PSG_LATCH_MASK
       | (i << 5)
      //  | (st->freq[i] & 0b0000'1111);
       | (st->freq[i] & PSG_DATA_MASK);
      
      fputc( 
        c, 
        fOUT
      );

      if (st->hi_freq_change[i])
      {
        // DATA byte needed?

        // make sure DATA bytes have 1 as 6th bit
        // unsigned char c = 0b0100'0000 | (st->freq[i] >> 4);
        unsigned char c = PSG_HI_CH_MASK | (st->freq[i] >> 4);
        fputc(
          c, 
          fOUT
        );
      }
    }

    if (st->volume_change[i])
    {
      // latch channel 0-2 volume
      unsigned char c = 
        // 0b1001'0000
        PSG_VOLUME_MASK
        | (i << 5) 
        // | (st->active[i] & 0b0000'1111);
        | (st->active[i] & PSG_DATA_MASK);

      fputc(
        c, 
        fOUT
      );
    }
  }

  if (st->freq_change[3])
  {
    // latch channel 3 (noise)
    unsigned char c = 
      // 0b1110'0000 
      PSG_NOISE_FREQ_MASK
      // | (st->freq[3] & 0b0000'0111);
      | (st->freq[3] & PSG_NOISE_FREQ_TYPE_MASK);
    
    fputc(
      c, 
      fOUT
    );
  }

  if (st->volume_change[3])
  {
    // latch channel 3 volume
    unsigned char c = 
      // 0b1111'0000 
      PSG_NOISE_VOLUME_MASK
      // | (st->volume[3] & 0b0000'1111);
      | (st->volume[3] & PSG_DATA_MASK);
    
    fputc(
      c,
      fOUT
    );
  }
}

// ポーズデータの出力
void dump_pause(
  FILE* fOUT, 
  // int *pause_len,
  // int *pause_started
  Context *ctx
)
{
  if (ctx->pause_len > 0)
  {
    while (ctx->pause_len > MAX_WAIT)
    {
      // write PSG_WAIT+7 to file
      fputc(
        PSG_WAIT + MAX_WAIT,  // 0x38 + 7
        fOUT
      );

      // skip MAX_WAIT+1
      ctx->pause_len -= MAX_WAIT + 1;
    }
    if (ctx->pause_len > 0)
    {
      // write PSG_WAIT+[0 to 7] to file, don't do it if 0
      fputc(
        PSG_WAIT + (ctx->pause_len - 1), // 0x38 + 
        fOUT
      );
    }
  }

  ctx->pause_len = 0;
  ctx->pause_started = FALSE;
}

void found_pause(
  FILE* fOUT,
  // int* frame_started,
  // int* pause_started,
  // PSGState *st
  Context *ctx
)
{
  if (ctx->frame_started)
  {
    // 1フレーム分の音データを出力する関数
    dump_frame(
      fOUT, 
      &ctx->st
      // ctx
    );

    // 初期化
    init_frame(&ctx->st);

    ctx->frame_started = FALSE;
  }

  ctx->pause_started = TRUE;
}

void empty_data(
  FILE* fOUT,
  // int *pause_len,
  // int *pause_started,
  // int *frame_started,
  // PSGState *st
  Context *ctx
)
{
  if (ctx->pause_started)
  {
    // ポーズデータの出力
    dump_pause(
      fOUT, 
      // pause_len,
      // pause_started
      ctx
    );
  }
  else if (ctx->frame_started)
  {
    dump_frame(
      fOUT,
      &ctx->st
    );

    init_frame(&ctx->st);
    ctx->frame_started = FALSE;
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
int checkSFX(
  int argc, 
  char *argv[],
  PSGState *st
)
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
        st->active[0] = TRUE;
        break;
      case '1':
        st->active[1] = TRUE;
        break;
      case '2':
        st->active[2] = TRUE;
        break;
      case '3':
        st->active[3] = TRUE;
        break;
      default:
        // 致命的エラー：オプションの第3パラメータには、0〜3の数字のみを含めることができます。
        printf("Fatal: the optional third parameter can only contains digits 0 to 3\n");
        return (1);
      }
    }

    if ( st->active[0] == FALSE
      || st->active[1] == FALSE
      || st->active[2] == FALSE
      || st->active[3] == FALSE
    )
    {
      // 1つでもFALSEならSFXとする
      is_sfx = TRUE;
      printf("Info: SFX conversion on channel(s): %s%s%s%s\n",
         st->active[0] ? "0" : "_",
         st->active[1] ? "1" : "_", 
         st->active[2] ? "2" : "_", 
         st->active[3] ? "3" : "_"
      );
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
  // int *pause_len,
  // int *pause_started,
  // int *frame_started
  Context *ctx
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
  // int *pause_len,
  // int *pause_started,
  // int *frame_started
  Context *ctx
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
  // int is_sfx,
  // int *first_byte,
  // int *loop_offset,
  // int *pause_started,
  // int *pause_len,
  // int *frame_started,
  // int *warn_32,
  // int *latched_chn,
  // unsigned char *lastlatch,
  // PSGState *st
  Context *ctx
)
{
  int input_data = gzgetc(fIN);

  if (input_data & 0b1000'0000)
  {
    // ラッチデータ
    ctx->lastlatch = input_data;

    // isolate chn number
    ctx->latched_chn = (input_data & 0b0110'0000) >> 5; 
  }
  else
  {
    // ラッチデータではない
    input_data |= 0b0100'0000;
  }

  if (
    (ctx->is_sfx == 0)
      || (ctx->st.active[ctx->latched_chn])
  )
  {
    // アクティブなチャンネル上でのみ出力する
    if (ctx->pause_started)
    {
      // ポーズデータの出力
      dump_pause(
        fOUT, 
        // ctx->pause_len,
        // ctx->pause_started
        ctx
      );
    }
    ctx->frame_started = TRUE;

    if (
      (ctx->first_byte) 
      && ((input_data & 0b1000'0000) == 0)
    )
    {
      add_command(
        ctx->lastlatch, // TODO input_data説
        // is_sfx,
        // warn_32,
        // *lastlatch,
        // st
        ctx
      );

      printf("Warning: added missing latch command in frame start\n");
    }

    add_command(
      input_data, 
      // is_sfx,
      // warn_32,
      // *lastlatch,
      // st
      ctx
    );

    ctx->first_byte = FALSE;
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
  // int *first_byte,
  // int *loop_offset,
  // int *pause_len,
  // int *pause_started,
  // int *frame_started,
  // PSGState *st
  Context *ctx
)
{
  found_pause(
    fOUT,
    // frame_started,
    // pause_started,
    // st
    ctx
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

  ctx->pause_len += fs;
  ctx->first_byte = TRUE;
}

void sampleSkip(
  gzFile fIN,
  FILE* fOUT,
  // int sample_divider,
  // int* pause_len,
  // int* loop_offset,
  // int* first_byte,
  // int* pause_started,
  // int* frame_started,
  // PSGState *st
  Context *ctx
)
{
  found_pause(
    fOUT, 
    // frame_started,
    // pause_started,
    // st
    ctx
  );

  int ss = gzgetc(fIN) + gzgetc(fIN) * 256;

  // samples to frames
  int fs = ss / ctx->sample_divider;

  if ((ss % ctx->sample_divider) != 0)
  {
    printf("Warning: pause length isn't perfectly frame sync'd\n");
    if ((ss % ctx->sample_divider) > (ctx->sample_divider / 2)) 
    {
      fs++;
    }
  }

  ctx->pause_len += fs;

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

  ctx->first_byte = TRUE;
}

void endOfData(
  FILE* fOUT,
  // int* pause_len,
  // int* loop_offset,
  // int* pause_started,
  // int* frame_started,
  // int* leave
  // PSGState *st
  Context *ctx
)
{
  // end of data
  // *leave = TRUE;
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
    // pause_len,
    // pause_started,
    // frame_started,
    // st
    ctx
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

  // PSGState st;
  Context ctx;

  // int first_byte = TRUE;
  // int pause_len = 0;  // 「待ち時間（ウェイト）の長さ」を蓄積するカウンタ
  // int frame_started = TRUE;
  // int pause_started = FALSE;
  // int warn_32 = FALSE;
  // int latched_chn = 0;
  // unsigned char lastlatch = 0b1001'1111;

  ctx.first_byte = TRUE;
  ctx.pause_len = 0;
  ctx.frame_started = TRUE;
  ctx.pause_started = FALSE;
  ctx.warn_32 = FALSE;
  ctx.lastlatch = 0b1001'1111;

  // argcのチェック
  if(checkArgc(argc))
  {
    // 引数の数が想定外の場合は終了
    return (1);
  }

  // SFXかどうか
  ctx.is_sfx = checkSFX(
    argc,
    argv,
    &ctx.st
  );

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
  ctx.sample_divider = getSampleDivider(fIN);

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

  while (
    (leave == FALSE) && 
    (!gzeof(fIN))  // ファイルが終わっているかどうか
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
        // printf("VGM_GGSTEREO\n");

        GGStereo(
          fIN,
          fOUT,
          // &loop_offset,
          // &pause_len,
          // &pause_started,
          // &frame_started
          &ctx
        );
        break;
      }
      case VGM_FMFOLLOWS: // 0x51
      {
        // printf("VGM_FMFOLLOWS\n");

        fmFollows(
          fIN, 
          fOUT, 
          // &loop_offset,
          // &pause_len,
          // &pause_started,
          // &frame_started 
          &ctx
        );
        break;
      }
      case VGM_PSGFOLLOWS:  // 0x50
      {
        // printf("VGM_PSGFOLLOWS\n");

        psgFollows(
          fIN, 
          fOUT, 
          // is_sfx,
          // &first_byte,
          // &loop_offset,
          // &pause_started,
          // &pause_len,
          // &frame_started,
          // &warn_32,
          // &latched_chn,
          // &lastlatch,
          // &st
          &ctx
        );
        break;
      }
      case VGM_FRAMESKIP_NTSC: // 0x62
      case VGM_FRAMESKIP_PAL:  // 0x63
      {
        // printf("VGM_FRAMESKIP_NTSC\n");

        frameSkip(
          fIN,
          fOUT,
          // &first_byte,
          // &loop_offset,
          // &pause_len,
          // &pause_started,
          // &frame_started,
          // &st
          &ctx
        );
        break;
      }
      case VGM_SAMPLESKIP:  // 0x61
      {
        // printf("VGM_SAMPLESKIP\n");

        // 待つ
        sampleSkip(
          fIN,
          fOUT,
          // sample_divider,
          // &pause_len,
          // &loop_offset,
          // &first_byte,
          // &pause_started,
          // &frame_started,
          // &st
          &ctx
        );

        break;
      }
      case VGM_ENDOFDATA: // 0x66
      {
        // printf("VGM_ENDOFDATA\n");

        endOfData(
          fOUT,
          // &pause_len,
          // &loop_offset,
          // &pause_started,
          // &frame_started,
          // &leave  // ループを抜けるためのフラグ
          // &st
          &ctx
        );

        leave = TRUE;
        break;
      }
      default:
      {
        // printf("default\n");

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
