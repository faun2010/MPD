/*
* Direct Stream Transfer (DST) codec
* ISO/IEC 14496-3 Part 3 Subpart 10: Technical description of lossless coding of oversampled audio
*/

#ifndef CT_ENC_H
#define CT_ENC_H

#include <stdint.h>
#include <array>
#include <vector>
#include "consts.h"
#include "ct.h"

using std::array;
using std::vector;

namespace dst {

	static auto BITMASK = [](auto data, auto bitnr) {
		return (uint8_t)(((data) >> (bitnr)) & 1);
	};

template<ct_e ct_type>
class ct_enc_t {
protected:
	static constexpr int ct_size = (ct_type == ct_e::Filter) ? MAXPREDORDER : AC_HISMAX;
	static constexpr int ct_bits = (ct_type == ct_e::Filter) ? MAXFILTERBITS : MAXPTABLEBITS;
	static constexpr int SizeCodedTableLen = (ct_type == ct_e::Filter) ? SIZE_CODEDPREDORDER : SIZE_CODEDPTABLELEN;
	static constexpr int EntryLen = (ct_type == ct_e::Filter) ? SIZE_PREDCOEF : AC_BITS - 1;
	static constexpr int MaxRiceM = (ct_type == ct_e::Filter) ? MAX_RICE_M_F : MAX_RICE_M_P;
	typedef array<int, ct_size> ct_table_t;
	typedef vector<ct_table_t> ct_tables_t;
public:
	int NrOfTables;                                 // Number of coded tables
	int StreamBits;                                 // Number of bits all filters use in the stream
	int CPredOrder[NROFFRICEMETHODS];               // Code_PredOrder[Method]
	int CPredCoef[NROFPRICEMETHODS][MAXCPREDORDER]; // Code_PredCoef[Method][CoefNr]
	vector<bool> Coded;                             // DST encode coefs/entries of Fir/PtabNr
	vector<int> BestMethod;                         // BestMethod[Fir/PtabNr]
	vector<array<int, NROFFRICEMETHODS>> m;         // m[Fir/PtabNr][Method]
	vector<int> DataLenData;                        // Fir/PtabDataLength[Fir/PtabNr]
	vector<array<int, ct_size>> data;               // Fir/PtabData[FirNr][Index]
public:
	ct_enc_t() {
		if constexpr(ct_type == ct_e::Filter) {
			CPredOrder[0] = 1;
			CPredCoef[0][0] = -8;
			CPredOrder[1] = 2;
			CPredCoef[1][0] = -16;
			CPredCoef[1][1] = 8;
			CPredOrder[2] = 3;
			CPredCoef[2][0] = -9;
			CPredCoef[2][1] = -5;
			CPredCoef[2][2] = 6;
#if NROFFRICEMETHODS == 4
			CPredOrder[3] = 1;
			CPredCoef[3][0] = 8;
#endif
		}
		if constexpr(ct_type == ct_e::Ptable) {
			CPredOrder[0] = 1;
			CPredCoef[0][0] = -8;
			CPredOrder[1] = 2;
			CPredCoef[1][0] = -16;
			CPredCoef[1][1] = 8;
			CPredOrder[2] = 3;
			CPredCoef[2][0] = -24;
			CPredCoef[2][1] = 24;
			CPredCoef[2][2] = -8;
		}
	}

	void init(int tables) {
		NrOfTables = tables;
		Coded.resize(tables);
		BestMethod.resize(tables);
		m.resize(tables);
		DataLenData.resize(tables);
		data.resize(tables);
	}

	int RiceRunLength(int Nr, int m) {
		int RunLength = 0;
		if (Nr != 0) {
			RunLength++;
		}
		if (Nr < 0) {
			Nr = -Nr;
		}
		RunLength += ((Nr >> m) + 1) + m;
		return RunLength;
	}

	void RiceEncode(uint8_t* EncodedFrame, int& BitNr, int Nr, int m) {
		int LSBs;
		int RunLength;
		int Sign;

		Sign = 0;
		if (Nr < 0) {
			Nr = -Nr;
			Sign = 1;
		}
		LSBs = ((1 << m) - 1) & Nr;
		RunLength = Nr >> m;
		for (auto i = 0; i < RunLength; i++) {
			AddBitToStream(EncodedFrame, 0, BitNr);
		}
		AddBitToStream(EncodedFrame, 1, BitNr);
		AddBitsToStream(EncodedFrame, m, LSBs, BitNr);
		if (Nr != 0) {
			AddBitToStream(EncodedFrame, Sign, BitNr);
		}
	}

	int FindBestMethod(int TableNr, ct_table_t& TableData, int TableSize) {
		Coded[TableNr] = 0;
		BestMethod[TableNr] = -1;
		int PlainLen = EntryLen * TableSize;
		int RiceBestMethod = -1;
		int TableBestLen = PlainLen;
		for (int RiceMethod = 0; RiceMethod < NROFFRICEMETHODS; RiceMethod++) {
			int RiceBestM = -1;
			int RiceBestMLen = -1;
			for (int RiceM = 0; RiceM <= MaxRiceM; RiceM++) {
				int RiceLen = SIZE_RICEMETHOD + EntryLen * CPredOrder[RiceMethod] + SIZE_RICEM;
				for (int EntryNr = CPredOrder[RiceMethod]; EntryNr < TableSize; EntryNr++) {
					int x = 0;
					int c = (ct_type == ct_e::Filter) ? TableData[EntryNr] : TableData[EntryNr] - 1;
					int r;
					for (int TapNr = 0; TapNr < CPredOrder[RiceMethod]; TapNr++) {
						x += CPredCoef[RiceMethod][TapNr] * ((ct_type == ct_e::Filter) ? TableData[EntryNr - TapNr - 1] : TableData[EntryNr - TapNr - 1] - 1);
					}
					if (x >= 0) {
						r = c + (x + 4) / 8;
					}
					else {
						r = c - (-x + 3) / 8;
					}
					RiceLen += RiceRunLength(r, RiceM);
				}
				if (RiceLen < RiceBestMLen || RiceBestMLen < 0) {
					RiceBestM = RiceM;
					RiceBestMLen = RiceLen;
				}
			}
			if (RiceBestMLen < TableBestLen || TableBestLen < 0) {
				RiceBestMethod = RiceMethod;
				TableBestLen = RiceBestMLen;
			}
			m[TableNr][RiceMethod] = RiceBestM;
		}
		if (TableBestLen < PlainLen) {
			Coded[TableNr] = 1;
			BestMethod[TableNr] = RiceBestMethod;
		}
		DataLenData[TableNr] = TableBestLen;
		return TableBestLen;
	}

	int AddTableToStream(unsigned char* EncodedData, int& BitNr, int TableNr, int* TableData, int TableSize) {
		AddBitsToStream(EncodedData, SizeCodedTableLen, TableSize - 1, BitNr);

		if ((ct_type == ct_e::Ptable) && TableSize == 1) {
			return BitNr;
		}

		/* Coded Bit */
		AddBitToStream(EncodedData, Coded[TableNr], BitNr);

		/* Table Entries */
		if (Coded[TableNr] == 0) {
			for (auto j = 0; j < TableSize; j++) {
				AddBitsToStream(EncodedData, EntryLen, (ct_type == ct_e::Filter) ? TableData[j] : TableData[j] - 1, BitNr);
			}
		}
		else {
			auto TableBestMethod = BestMethod[TableNr];
			AddBitsToStream(EncodedData, SIZE_RICEMETHOD, TableBestMethod, BitNr);
			for (int CoefNr = 0; CoefNr < CPredOrder[TableBestMethod]; CoefNr++) {
				AddBitsToStream(EncodedData, EntryLen, (ct_type == ct_e::Filter) ? TableData[CoefNr] : TableData[CoefNr] - 1, BitNr);
			}
			AddBitsToStream(EncodedData, SIZE_RICEM, m[TableNr][TableBestMethod], BitNr);
			for (int EntryNr = CPredOrder[TableBestMethod]; EntryNr < TableSize; EntryNr++) {
				int x = 0;
				int c = (ct_type == ct_e::Filter) ? TableData[EntryNr] : TableData[EntryNr] - 1;
				int r;
				for (int TapNr = 0; TapNr < CPredOrder[TableBestMethod]; TapNr++) {
					x += CPredCoef[TableBestMethod][TapNr] * ((ct_type == ct_e::Filter) ? TableData[EntryNr - TapNr - 1] : TableData[EntryNr - TapNr - 1] - 1);
				}
				if (x >= 0) {
					r = c + (x + 4) / 8;
				}
				else {
					r = c - (-x + 3) / 8;
				}
				RiceEncode(EncodedData, BitNr, r, m[TableNr][TableBestMethod]);
			}
		}
		return BitNr;
	}

	void AddBitToStream(unsigned char* EncodedData, int DataBit, int& BitNr) {
		EncodedData[BitNr] = DataBit & 1;
		BitNr++;
	}

	void AddBitsToStream(unsigned char* EncodedData, int BitCount, unsigned int DataBits, int& BitNr) {
		for (int k = BitCount - 1; k >= 0; k--) {
			AddBitToStream(EncodedData, BITMASK(DataBits, k), BitNr);
		}
	}

	int encode(ct_tables_t& TablesData, vector<int>& TablesSize, unsigned char* EncodedFrame) {
		int BitNr = 0;
		for (auto TableNr = 0; TableNr < NrOfTables; TableNr++) {
			FindBestMethod(TableNr, TablesData[TableNr], TablesSize[TableNr]);
			AddTableToStream(EncodedFrame, BitNr, TableNr, TablesData[TableNr].data(), TablesSize[TableNr]);
		}
		return BitNr;
	}
};

typedef ct_enc_t<ct_e::Filter> ft_enc_t;
typedef ct_enc_t<ct_e::Ptable> pt_enc_t;

}

#endif
