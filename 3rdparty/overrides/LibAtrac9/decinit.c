// LibAtrac9's C/src/decinit.c (MIT License, Copyright (c) 2018 Alex Barney: 3rdparty/LibAtrac9/LICENSE) with the
// decoder's shared lookup tables initialized once for all handles: the build takes it instead of the submodule's
// (3rdparty/CMakeLists.txt). Upstream (ec8899d) rewrites them on every InitDecoder while other threads decode with them.

#include "decinit.h"
#include "bit_allocation.h"
#include "bit_reader.h"
#include "error_codes.h"
#include "huffCodes.h"
#include "structures.h"
#include "tables.h"
#include "utility.h"
#include <math.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static At9Status InitConfigData(ConfigData* config, unsigned char * configData);
static At9Status ReadConfigData(ConfigData* config);
static At9Status InitFrame(Atrac9Handle* handle);
static At9Status InitBlock(Block* block, Frame* parentFrame, int blockIndex);
static At9Status InitChannel(Channel* channel, Block* parentBlock, int channelIndex);
static void InitHuffmanCodebooks();
static void InitHuffmanSet(const HuffmanCodebook* codebooks, int count);
static void GenerateTrigTables(int sizeBits);
static void GenerateShuffleTable(int sizeBits);
static void InitMdctTables();
static void GenerateMdctWindow(int frameSizePower);
static void GenerateImdctWindow(int frameSizePower);
static void InitSharedTables();

static int BlockTypeToChannelCount(BlockType blockType);

#ifdef _WIN32
static INIT_ONCE SharedTablesOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitSharedTablesOnce(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
	(void)once;
	(void)parameter;
	(void)context;
	InitSharedTables();
	return TRUE;
}
#else
static pthread_once_t SharedTablesOnce = PTHREAD_ONCE_INIT;
#endif

At9Status InitDecoder(Atrac9Handle* handle, unsigned char* configData, int wlength)
{
	ERROR_CHECK(InitConfigData(&handle->Config, configData));
	ERROR_CHECK(InitFrame(handle));
	// These tables are shared by every handle, including handles being decoded on
	// other threads. Publish all supported sizes once; reinitializing a decoder
	// must reset its own state without rewriting tables that another decoder reads.
#ifdef _WIN32
	if (!InitOnceExecuteOnce(&SharedTablesOnce, InitSharedTablesOnce, NULL, NULL))
#else
	if (pthread_once(&SharedTablesOnce, InitSharedTables) != 0)
#endif
	{
		return ERR_NOT_IMPLEMENTED;
	}
	handle->Wlength = wlength;
	handle->Initialized = 1;
	return ERR_SUCCESS;
}

static At9Status InitConfigData(ConfigData* config, unsigned char* configData)
{
	memcpy(config->ConfigData, configData, CONFIG_DATA_SIZE);
	ERROR_CHECK(ReadConfigData(config));

	config->FramesPerSuperframe = 1 << config->SuperframeIndex;
	config->SuperframeBytes = config->FrameBytes << config->SuperframeIndex;

	config->ChannelConfig = ChannelConfigs[config->ChannelConfigIndex];
	config->ChannelCount = config->ChannelConfig.ChannelCount;
	config->SampleRate = SampleRates[config->SampleRateIndex];
	config->HighSampleRate = config->SampleRateIndex > 7;
	config->FrameSamplesPower = SamplingRateIndexToFrameSamplesPower[config->SampleRateIndex];
	config->FrameSamples = 1 << config->FrameSamplesPower;
	config->SuperframeSamples = config->FrameSamples * config->FramesPerSuperframe;

	return ERR_SUCCESS;
}

static At9Status ReadConfigData(ConfigData* config)
{
	BitReaderCxt br;
	InitBitReaderCxt(&br, &config->ConfigData);

	const int header = ReadInt(&br, 8);
	config->SampleRateIndex = ReadInt(&br, 4);
	config->ChannelConfigIndex = ReadInt(&br, 3);
	const int validationBit = ReadInt(&br, 1);
	config->FrameBytes = ReadInt(&br, 11) + 1;
	config->SuperframeIndex = ReadInt(&br, 2);

	if (header != 0xFE || validationBit != 0)
	{
		return ERR_BAD_CONFIG_DATA;
	}

	return ERR_SUCCESS;
}

static At9Status InitFrame(Atrac9Handle* handle)
{
	const int blockCount = handle->Config.ChannelConfig.BlockCount;
	handle->Frame.Config = &handle->Config;
	int channelNum = 0;

	for (int i = 0; i < blockCount; i++)
	{
		ERROR_CHECK(InitBlock(&handle->Frame.Blocks[i], &handle->Frame, i));

		for (int c = 0; c < handle->Frame.Blocks[i].ChannelCount; c++)
		{
			handle->Frame.Channels[channelNum++] = &handle->Frame.Blocks[i].Channels[c];
		}
	}

	return ERR_SUCCESS;
}

static At9Status InitBlock(Block* block, Frame* parentFrame, int blockIndex)
{
	block->Frame = parentFrame;
	block->BlockIndex = blockIndex;
	block->Config = parentFrame->Config;
	block->BlockType = block->Config->ChannelConfig.Types[blockIndex];
	block->ChannelCount = BlockTypeToChannelCount(block->BlockType);

	for (int i = 0; i < block->ChannelCount; i++)
	{
		ERROR_CHECK(InitChannel(&block->Channels[i], block, i));
	}

	return ERR_SUCCESS;
}

static At9Status InitChannel(Channel* channel, Block* parentBlock, int channelIndex)
{
	channel->Block = parentBlock;
	channel->Frame = parentBlock->Frame;
	channel->Config = parentBlock->Config;
	channel->ChannelIndex = channelIndex;
	channel->Mdct.Bits = parentBlock->Config->FrameSamplesPower;
	return ERR_SUCCESS;
}

static void InitHuffmanCodebooks()
{
	InitHuffmanSet(HuffmanScaleFactorsUnsigned, sizeof(HuffmanScaleFactorsUnsigned) / sizeof(HuffmanCodebook));
	InitHuffmanSet(HuffmanScaleFactorsSigned, sizeof(HuffmanScaleFactorsSigned) / sizeof(HuffmanCodebook));
	InitHuffmanSet((HuffmanCodebook*)HuffmanSpectrum, sizeof(HuffmanSpectrum) / sizeof(HuffmanCodebook));
}

static void InitHuffmanSet(const HuffmanCodebook* codebooks, int count)
{
	for (int i = 0; i < count; i++)
	{
		InitHuffmanCodebook(&codebooks[i]);
	}
}

static void InitSharedTables()
{
	InitMdctTables();
	InitHuffmanCodebooks();
	GenerateGradientCurves();
}

static void InitMdctTables()
{
	for (int i = 0; i < 9; i++)
	{
		GenerateTrigTables(i);
		GenerateShuffleTable(i);
	}
	for (int frameSizePower = 6; frameSizePower <= 8; frameSizePower++)
	{
		GenerateMdctWindow(frameSizePower);
		GenerateImdctWindow(frameSizePower);
	}
}

static void GenerateTrigTables(int sizeBits)
{
	const int size = 1 << sizeBits;
	double* sinTab = SinTables[sizeBits];
	double* cosTab = CosTables[sizeBits];

	for (int i = 0; i < size; i++)
	{
		const double value = M_PI * (4 * i + 1) / (4 * size);
		sinTab[i] = sin(value);
		cosTab[i] = cos(value);
	}
}

static void GenerateShuffleTable(int sizeBits)
{
	const int size = 1 << sizeBits;
	int* table = ShuffleTables[sizeBits];

	for (int i = 0; i < size; i++)
	{
		table[i] = BitReverse32(i ^ (i / 2), sizeBits);
	}
}

static void GenerateMdctWindow(int frameSizePower)
{
	const int frameSize = 1 << frameSizePower;
	double* mdct = MdctWindow[frameSizePower - 6];

	for (int i = 0; i < frameSize; i++)
	{
		mdct[i] = (sin(((i + 0.5) / frameSize - 0.5) * M_PI) + 1.0) * 0.5;
	}
}

static void GenerateImdctWindow(int frameSizePower)
{
	const int frameSize = 1 << frameSizePower;
	double* imdct = ImdctWindow[frameSizePower - 6];
	double* mdct = MdctWindow[frameSizePower - 6];

	for (int i = 0; i < frameSize; i++)
	{
		imdct[i] = mdct[i] / (mdct[frameSize - 1 - i] * mdct[frameSize - 1 - i] + mdct[i] * mdct[i]);
	}
}

static int BlockTypeToChannelCount(BlockType blockType)
{
	switch (blockType)
	{
	case Mono:
		return 1;
	case Stereo:
		return 2;
	case LFE:
		return 1;
	default:
		return 0;
	}
}
