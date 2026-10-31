#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGTD_MAX_RECORDS 2048u
#define PGTD_PACKET_SLOTS 2048u
#define PGTD_TOKEN_CAPACITY 1024u
#define PGTD_TAG_BUDGET 8192u
#define PGTD_SCOPE_MAX_DEPTH 16u

#define PGTD_SCOPE_FLAG_KEY_VALID UINT32_C(0x00000001)
#define PGTD_TAG_FLAG_KEY_VALID UINT32_C(0x40000000)
#define PGTD_TAG_FLAG_ENTITY_UNKNOWN UINT32_C(0x20000000)
#define PGTD_TAG_FLAG_CAMERA_INVALID UINT32_C(0x80000000)

typedef struct PGTDCamera {
	uint64_t rawViewport;
	uint32_t valid;
	uint32_t width;
	uint32_t height;
	float view[16];
	float projection[16];
	float nearPlane;
	float farPlane;
} PGTDCamera;

typedef struct PGTDTag {
	uint64_t session;
	uint64_t sequence;
	uint32_t producerThread;
	uint32_t stage;
	uint32_t entityClass;
	uint32_t entityKey;
	uint32_t entityToken;
	uint32_t auxiliaryToken;
	uint32_t rootMatrixToken;
	uint32_t paletteToken;
	uint32_t viewportToken;
	uint32_t scopeDepth;
	uint32_t flags;
	PGTDCamera camera;
} PGTDTag;

typedef struct PGTDMetalSample {
	PGTDTag tag;
	uint32_t frame;
	uint32_t drawKind;
	uint32_t opcode;
	uint32_t indexCount;
	uint32_t instanceCount;
	uint32_t vsToken;
	uint32_t psToken;
	uint32_t layoutToken;
	uint32_t depthToken;
	uint32_t depthFormat;
	uint32_t depthWidth;
	uint32_t depthHeight;
	uint32_t depthSamples;
	uint32_t depthMip;
	uint32_t depthSlice;
	uint32_t presentedToken;
	uint32_t presentedWidth;
	uint32_t presentedHeight;
	uint32_t presentedFormat;
	uint32_t colorToken[4];
	uint32_t colorFormat[4];
	uint32_t colorWidth[4];
	uint32_t colorHeight[4];
	uint32_t colorSamples[4];
	float viewport[4];
	uint32_t uniformToken[4];
	uint32_t uniformBytes[4];
	uint32_t uniformFirst[4];
	uint64_t uniformHash[4];
	uint32_t srvToken[4];
	uint32_t srvSlot[4];
	uint32_t srvFormat[4];
	uint32_t srvWidth[4];
	uint32_t srvHeight[4];
	float wvp[16];
	uint32_t flags;
} PGTDMetalSample;

typedef struct PGTDStats {
	uint64_t claimed;
	uint64_t published;
	uint64_t matched;
	uint64_t misses;
	uint64_t collisions;
	uint64_t overflow;
	uint64_t invalidCamera;
	uint64_t scopeErrors;
	uint64_t tokenOverflow;
	uint64_t recordOverflow;
	uint64_t invalidRecord;
	uint64_t threadOverflow;
	uint64_t truncatedRecords;
} PGTDStats;

typedef struct PGTDReport {
	uint64_t session;
	uint32_t frameCount;
	uint32_t recordCount;
	PGTDStats stats;
	PGTDMetalSample samples[PGTD_MAX_RECORDS];
} PGTDReport;

int PGTDEnabled(void);
uint64_t PGTDBegin(void);
void PGTDEnd(void);
// candidateKey is a full-width raw identifier when KEY_VALID is set; reports
// contain only its session-local anonymous token, never the raw value.
uint64_t PGTDScopeEnter(uint32_t kind, uint64_t identity, uint64_t auxiliary,
		uint64_t candidateKey, uint32_t flags);
void PGTDScopeLeave(uint64_t cookie);
void PGTDInvalidateThread(void);

// op 32 is DRAW; it is sampled every 128 draws, at the first draw after a stage
// change, and for the bounded selected entity keys. op 26 claims once per stage
// change while a graphics stage is active. op 7 is PRESENT and always claims.
int PGTDClaimPacket(uint32_t op);
void PGTDProducerPacket(uint64_t packetAddress, uint32_t op, uint32_t words,
		const PGTDCamera *camera);
int PGTDConsumePacket(uint64_t packetAddress, uint32_t op, uint32_t words,
		PGTDTag *out);

uint32_t PGTDToken(uint32_t domain, uint64_t raw);
void PGTDForgetToken(uint32_t domain, uint64_t raw);
void PGTDRecordMetal(const PGTDMetalSample *sample);
uint32_t PGTDNextFrame(void);
int PGTDCopyReport(PGTDReport *out);

#ifdef __cplusplus
}
#endif
