#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <iosfwd>
#include <stdint.h>
#include <memory.h>
#include "../mcl/src/bn_c_impl.hpp"
#define BLS_DLL_EXPORT

#include <bls/bls.h>
/*
	BLS signature
	e : G1 x G2 -> Fp12
	Q in G2 ; fixed global parameter
	H : {str} -> G1
	s : secret key
	sQ ; public key
	s H(m) ; signature of m
	verify ; e(sQ, H(m)) = e(Q, s H(m))
*/

static G2 g_Q;
static std::vector<Fp6> g_Qcoeff; // precomputed Q
static const G2& getQ() { return g_Q; }
static const std::vector<Fp6>& getQcoeff() { return g_Qcoeff; }

int blsInitNotThreadSafe(int curve, int maxUnitSize)
	try
{
	if (mclBn_init(curve, maxUnitSize) != 0) return -1;
	BN::mapToG2(g_Q, 1);
	BN::precomputeG2(g_Qcoeff, getQ());
	return 0;
} catch (std::exception&) {
	return -1;
}

#ifndef __EMSCRIPTEN__
	#if defined(CYBOZU_CPP_VERSION) && CYBOZU_CPP_VERSION >= CYBOZU_CPP_VERSION_CPP11
	#include <mutex>
		#define USE_STD_MUTEX
	#else
	#include <cybozu/mutex.hpp>
		#define USE_CYBOZU_MUTEX
	#endif
#endif

int blsInit(int curve, int maxUnitSize)
{
	int ret = 0;
#ifdef USE_STD_MUTEX
	static std::mutex m;
	std::lock_guard<std::mutex> lock(m);
#elif defined(USE_CYBOZU_MUTEX)
	static cybozu::Mutex m;
	cybozu::AutoLock lock(m);
#endif
	static int g_curve = -1;
	if (g_curve != curve) {
		ret = blsInitNotThreadSafe(curve, maxUnitSize);
		g_curve = curve;
	}
	return ret;
}

static inline const mclBnG1 *cast(const G1* x) { return (const mclBnG1*)x; }
static inline const mclBnG2 *cast(const G2* x) { return (const mclBnG2*)x; }

/*
	e(P1, Q1) == e(P2, Q2)
	<=> finalExp(ML(P1, Q1)) == finalExp(ML(P2, Q2))
	<=> finalExp(ML(P1, Q1) / ML(P2, Q2)) == 1
	<=> finalExp(ML(P1, Q1) * ML(-P2, Q2)) == 1
	Q1 is precomputed
*/
bool isEqualTwoPairings(const G1& P1, const Fp6* Q1coeff, const G1& P2, const G2& Q2)
{
	std::vector<Fp6> Q2coeff;
	BN::precomputeG2(Q2coeff, Q2);
	Fp12 e;
	BN::precomputedMillerLoop2(e, P1, Q1coeff, -P2, Q2coeff.data());
	BN::finalExp(e, e);
	return e.isOne();
}

mclSize checkAndCopy(char *buf, mclSize maxBufSize, const std::string& s)
{
	if (s.size() > maxBufSize + 1) {
		return 0;
	}
	memcpy(buf, s.c_str(), s.size());
	buf[s.size()] = '\0';
	return s.size();
}

mclSize blsGetOpUnitSize() // FpUint64Size
{
	return Fp::getUnitSize() * sizeof(mcl::fp::Unit) / sizeof(uint64_t);
}

int blsGetCurveOrder(char *buf, mclSize maxBufSize)
	try
{
	std::string s;
	Fr::getModulo(s);
	return (int)checkAndCopy(buf, maxBufSize, s);
} catch (std::exception&) {
	return 0;
}

int blsGetFieldOrder(char *buf, mclSize maxBufSize)
	try
{
	std::string s;
	Fp::getModulo(s);
	return (int)checkAndCopy(buf, maxBufSize, s);
} catch (std::exception&) {
	return 0;
}

void blsGetGeneratorOfG2(blsPublicKey *pub)
{
	*(G2*)pub = getQ();
}

void blsGetPublicKey(blsPublicKey *pub, const blsSecretKey *sec)
{
	mclBnG2_mul(&pub->v, cast(&getQ()), &sec->v);
}
void blsSign(blsSignature *sig, const blsSecretKey *sec, const void *m, mclSize size)
{
	G1 Hm;
	BN::hashAndMapToG1(Hm, m, size);
	mclBnG1_mulCT(&sig->v, cast(&Hm), &sec->v);
}
int blsSecretKeyShare(blsSecretKey *sec, const blsSecretKey* msk, mclSize k, const blsId *id)
{
	return mclBn_FrEvaluatePolynomial(&sec->v, &msk->v, k, &id->v);
}

int blsSecretKeyRecover(blsSecretKey *sec, const blsSecretKey *secVec, const blsId *idVec, mclSize n)
{
	return mclBn_FrLagrangeInterpolation(&sec->v, &idVec->v, &secVec->v, n);
}

void blsGetPop(blsSignature *sig, const blsSecretKey *sec)
{
	blsPublicKey pub;
	blsGetPublicKey(&pub, sec);
	char buf[1024];
	mclSize n = mclBnG2_serialize(buf, sizeof(buf), &pub.v);
	assert(n);
	blsSign(sig, sec, buf, n);
}
int blsPublicKeyShare(blsPublicKey *pub, const blsPublicKey *mpk, mclSize k, const blsId *id)
{
	return mclBn_G2EvaluatePolynomial(&pub->v, &mpk->v, k, &id->v);
}
int blsPublicKeyRecover(blsPublicKey *pub, const blsPublicKey *pubVec, const blsId *idVec, mclSize n)
{
	return mclBn_G2LagrangeInterpolation(&pub->v, &idVec->v, &pubVec->v, n);
}
int blsSignatureRecover(blsSignature *sig, const blsSignature *sigVec, const blsId *idVec, mclSize n)
{
	return mclBn_G1LagrangeInterpolation(&sig->v, &idVec->v, &sigVec->v, n);
}

int blsVerify(const blsSignature *sig, const blsPublicKey *pub, const void *m, mclSize size)
{
	G1 Hm;
	BN::hashAndMapToG1(Hm, m, size);
	/*
		e(sHm, Q) = e(Hm, sQ)
		e(sig, Q) = e(Hm, pub)
	*/
	return isEqualTwoPairings(*cast(&sig->v), getQcoeff().data(), Hm, *cast(&pub->v));
}

int blsVerifyPop(const blsSignature *sig, const blsPublicKey *pub)
{
	char buf[1024];
	mclSize n = mclBnG2_serialize(buf, sizeof(buf), &pub->v);
	assert(n);
	return blsVerify(sig, pub, buf, n);
}

void blsIdSetInt(blsId *id, int x)
{
	mclBnFr_setInt(&id->v, x);
}
mclSize blsIdSerialize(void *buf, mclSize maxBufSize, const blsId *id)
{
	return mclBnFr_serialize(buf, maxBufSize, &id->v);
}
mclSize blsSecretKeySerialize(void *buf, mclSize maxBufSize, const blsSecretKey *sec)
{
	return mclBnFr_serialize(buf, maxBufSize, &sec->v);
}
mclSize blsPublicKeySerialize(void *buf, mclSize maxBufSize, const blsPublicKey *pub)
{
	return mclBnG2_serialize(buf, maxBufSize, &pub->v);
}
mclSize blsSignatureSerialize(void *buf, mclSize maxBufSize, const blsSignature *sig)
{
	return mclBnG1_serialize(buf, maxBufSize, &sig->v);
}
mclSize blsIdDeserialize(blsId *id, const void *buf, mclSize bufSize)
{
	return mclBnFr_deserialize(&id->v, buf, bufSize);
}
mclSize blsSecretKeyDeserialize(blsSecretKey *sig, const void *buf, mclSize bufSize)
{
	return mclBnFr_deserialize(&sig->v, buf, bufSize);
}
mclSize blsPublicKeyDeserialize(blsPublicKey *pub, const void *buf, mclSize bufSize)
{
	return mclBnG2_deserialize(&pub->v, buf, bufSize);
}
mclSize blsSignatureDeserialize(blsSignature *sig, const void *buf, mclSize bufSize)
{
	return mclBnG1_deserialize(&sig->v, buf, bufSize);
}
int blsIdIsEqual(const blsId *lhs, const blsId *rhs)
{
	return mclBnFr_isEqual(&lhs->v, &rhs->v);
}
int blsSecretKeyIsEqual(const blsSecretKey *lhs, const blsSecretKey *rhs)
{
	return mclBnFr_isEqual(&lhs->v, &rhs->v);
}
int blsPublicKeyIsEqual(const blsPublicKey *lhs, const blsPublicKey *rhs)
{
	return mclBnG2_isEqual(&lhs->v, &rhs->v);
}
int blsSignatureIsEqual(const blsSignature *lhs, const blsSignature *rhs)
{
	return mclBnG1_isEqual(&lhs->v, &rhs->v);
}
void blsSecretKeyAdd(blsSecretKey *sec, const blsSecretKey *rhs)
{
	mclBnFr_add(&sec->v, &sec->v, &rhs->v);
}
void blsSignatureAdd(blsSignature *sig, const blsSignature *rhs)
{
	mclBnG1_add(&sig->v, &sig->v, &rhs->v);
}
void blsPublicKeyAdd(blsPublicKey *pub, const blsPublicKey *rhs)
{
	mclBnG2_add(&pub->v, &pub->v, &rhs->v);
}
int blsIdSetLittleEndian(blsId *id, const void *buf, mclSize bufSize)
{
	return mclBnFr_setLittleEndian(&id->v, buf, bufSize);
}
int blsIdSetDecStr(blsId *id, const char *buf, mclSize bufSize)
{
	return mclBnFr_setStr(&id->v, buf, bufSize, 10);
}
int blsIdSetHexStr(blsId *id, const char *buf, mclSize bufSize)
{
	return mclBnFr_setStr(&id->v, buf, bufSize, 16);
}
mclSize blsIdGetLittleEndian(void *buf, mclSize maxBufSize, const blsId *id)
{
	return mclBnFr_serialize(buf, maxBufSize, &id->v);
}
mclSize blsIdGetDecStr(char *buf, mclSize maxBufSize, const blsId *id)
{
	return mclBnFr_getStr(buf, maxBufSize, &id->v, 10);
}
mclSize blsIdGetHexStr(char *buf, mclSize maxBufSize, const blsId *id)
{
	return mclBnFr_getStr(buf, maxBufSize, &id->v, 16);
}
int blsSecretKeySetLittleEndian(blsSecretKey *sec, const void *buf, mclSize bufSize)
{
	return mclBnFr_setLittleEndian(&sec->v, buf, bufSize);
}
int blsSecretKeySetDecStr(blsSecretKey *sec, const char *buf, mclSize bufSize)
{
	return mclBnFr_setStr(&sec->v, buf, bufSize, 10);
}
int blsSecretKeySetHexStr(blsSecretKey *sec, const char *buf, mclSize bufSize)
{
	return mclBnFr_setStr(&sec->v, buf, bufSize, 16);
}
mclSize blsSecretKeyGetLittleEndian(void *buf, mclSize maxBufSize, const blsSecretKey *sec)
{
	return mclBnFr_serialize(buf, maxBufSize, &sec->v);
}
mclSize blsSecretKeyGetDecStr(char *buf, mclSize maxBufSize, const blsSecretKey *sec)
{
	return mclBnFr_getStr(buf, maxBufSize, &sec->v, 10);
}
mclSize blsSecretKeyGetHexStr(char *buf, mclSize maxBufSize, const blsSecretKey *sec)
{
	return mclBnFr_getStr(buf, maxBufSize, &sec->v, 16);
}
int blsHashToSecretKey(blsSecretKey *sec, const void *buf, mclSize bufSize)
{
	return mclBnFr_setHashOf(&sec->v, buf, bufSize);
}
int blsSecretKeySetByCSPRNG(blsSecretKey *sec)
{
	return mclBnFr_setByCSPRNG(&sec->v);
}
int blsPublicKeySetHexStr(blsPublicKey *pub, const char *buf, mclSize bufSize)
{
	return mclBnG2_setStr(&pub->v, buf, bufSize, 16);
}
mclSize blsPublicKeyGetHexStr(char *buf, mclSize maxBufSize, const blsPublicKey *pub)
{
	return mclBnG2_getStr(buf, maxBufSize, &pub->v, 16);
}
int blsSignatureSetHexStr(blsSignature *sig, const char *buf, mclSize bufSize)
{
	return mclBnG1_setStr(&sig->v, buf, bufSize, 16);
}
mclSize blsSignatureGetHexStr(char *buf, mclSize maxBufSize, const blsSignature *sig)
{
	return mclBnG1_getStr(buf, maxBufSize, &sig->v, 16);
}
void blsDHKeyExchange(blsPublicKey *out, const blsSecretKey *sec, const blsPublicKey *pub)
{
	mclBnG2_mulCT(&out->v, &pub->v, &sec->v);
}

