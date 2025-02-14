
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "slave.h"
#include "my_slave.h"
#include <assert.h>

#include "swstruct.h"

#define COND_INTEGRATION_SCALE   2


 #define REG_SYNR(mask) \
  asm volatile ("synr %0"::"r"(mask))
 #define REG_SYNC(mask) \
  asm volatile ("sync %0"::"r"(mask))

__thread_local volatile unsigned int reply,rpl[8];
__thread_local volatile unsigned int st0;
__thread_local volatile unsigned int cdma,cspike;
__thread_local volatile unsigned int NSgroup_slave;
__thread_local swInfo_t swInfo;
__thread_local neurInfo_t *nInfo;
__thread_local synInfo_t *sInfo;
__thread_local int time_test=0;
__thread_local dma_desc dma_get_syn;
__thread_local dma_desc dma_get;
__thread_local spikeTime_t *firingTable;
__thread_local spikeTime_t *firingTable_mpi;
__thread_local float *ringBuffer;
__thread_local float *neuronPara;
__thread_local int simTime,sliceTime;
__thread_local int lenRB,offset;
__thread_local int lenRB_mpi,offset_mpi;
__thread_local int lenST,topST,endST,usedST,numST[20];
__thread_local volatile int lenST_mpi,topST_mpi,endST_mpi,usedST_mpi,numST_mpi[20];
__thread_local volatile int t1;
__thread_local volatile long dmatimes;
__thread_local float Izh_a,Izh_b,Izh_c,Izh_d;
__thread_local int recovery,voltage,gAMPA,gNMDA_d,gGABAa,gGABAb_d,neuronSizeN;

extern volatile unsigned int dma[64],spike[64],NS_group,NSall,numSpike[64];
extern float currentfactor;
extern int rank ,nproc;
static void syndma(spikeTime_t st,synInfo_t *sInfoLc);
static void syndma2(spikeTime_t st,synInfo_t *sInfoLc);
static void put_get_syn(synInfo_t *sInfoLc);
static void generatePostSpike(spikeTime_t st,synInfo_t *sInfoLc);
static void generatePostSpike_simd(spikeTime_t st,synInfo_t *sInfoLc);
static int addSpikeToTable(int i);
static int addSpikeToTable_simd(int,int);
static int addSpikeToTable_mpi(int i);
static int addSpikeToTable_simd_mpi(int,int);
static void decayConduct(void *ptr);
static void neuronUpdate_simd();
static void neuronUpdate_simd_wzc();
static void CurrentUpdate(void *ptr);
static void CurrentUpdate_mpi(void *ptr);
static void PoisCurrentUpdate(void *ptr);
static int SpikeDmaWrite_mpi(ptr);//mpi
static int SpikeDmaRead_mpi(ptr);//mpi
static void InputCurrent(float wt, float nspike);
#define dvdtIzh_simd(v,u,tmpI,h) (((v0_04*(v)+v5_0)*(v)+v140_0-(u)+(tmpI))*(h))
#define dudtIzh_simd(v,u,a,b,h) ((a)*((b)*(v)-(u))*(h))
inline float dvdtIzh(float v, float u, float tmpI, float h);
inline float dudtIzh(float v, float u, float a, float b, float h);

inline float dvdtIzh(float v, float u, float tmpI, float h) {
	return (((0.04*v+5.0)*v+140.0-u+tmpI)*h);
}

// single integration step for recovery equation of 4-param Izhikevich
inline float dudtIzh(float v, float u, float a, float b, float h) {
 	return (a*(b*v-u)*h);
}
void initSW(swInfo_t *ptr){

	int i;t1=0;dmatimes=0;
	Izh_a=0.02;Izh_b=0.2;Izh_c=-65;Izh_d=8.0;
	cdma=0;cspike=0;
	simTime=0,sliceTime=0;lenRB=0;
	lenRB_mpi=0;
	lenST=0; topST=0; endST=0; usedST=0;
	lenST_mpi=0; topST_mpi=0; endST_mpi=0; usedST_mpi=0;
	for(i=0;i<20;i++) numST[i]=0;
	for(i=0;i<20;i++) numST_mpi[i]=0;

	reply=0;
	athread_get(PE_MODE,&ptr[_MYID],&swInfo,sizeof(swInfo_t),&reply,0,0,0);
	while(reply!=1);

	int SizeN=swInfo.SizeN;
	int MaxN=swInfo.MaxN;

	lenRB=2*swInfo.Ndt; offset=0;
	lenRB_mpi=2*swInfo.Ndt; offset_mpi=0;
	lenST_mpi=SizeN+64;//spikeTime buffer

	if(_MYID==NTh-1) {lenST+=100;}
	if(_MYID==NTh-1) {lenST_mpi+=100;}
	/*****allocate mem********/

	nInfo=(neurInfo_t*)ldm_malloc(SizeN*sizeof(neurInfo_t)); //?????
	sInfo=(synInfo_t*)ldm_malloc((64*swInfo.Ndma+5)*sizeof(synInfo_t));
	firingTable_mpi=(spikeTime_t*)ldm_malloc(lenST_mpi*sizeof(spikeTime_t));
	ringBuffer=(float*)ldm_malloc(lenRB*SizeN*sizeof(float));
	neuronSizeN = (SizeN/4+1)*4;
	neuronPara=(float*)ldm_malloc(6*neuronSizeN*sizeof(float));
	assert(sizeof(spikeTime_t)==4);
	assert(sizeof(synInfo_t)==8);
	for(i=0;i<lenST;i++){
		firingTable_mpi[i].time=0xFFFF;
		firingTable_mpi[i].nid =0xFFFF;
	}
	for(i=0;i<lenRB*SizeN;i++){ringBuffer[i] =0.;}
	memset(nInfo,0,SizeN*sizeof(neurInfo_t));//init zero

	dma_set_size(&dma_get_syn,swInfo.Ndma*sizeof(synInfo_t));
	dma_set_op(&dma_get_syn,DMA_GET);
	dma_set_reply(&dma_get_syn,&reply);
	dma_set_mode(&dma_get_syn,PE_MODE);
	dma_set_bsize(&dma_get_syn,0);
	dma_set_stepsize(&dma_get_syn,0);

	dma_set_size(&dma_get,SizeN*sizeof(neurInfo_t));//need reset
	dma_set_op(&dma_get,DMA_GET);
	dma_set_reply(&dma_get,&reply);
	dma_set_mode(&dma_get,PE_MODE);
	dma_set_bsize(&dma_get,0);
	dma_set_stepsize(&dma_get,0);

	reply=0;
	dma(dma_get,&(swInfo.nInfoHost[swInfo.StartN-swInfo.gStart]),&nInfo[0]);
    dma_wait(&reply,1);
	 
	recovery=0;voltage=1;gAMPA=2;gNMDA_d=3;gGABAa=4;gGABAb_d=5;

	for(i=0;i<SizeN;i++){
		neuronPara[recovery*neuronSizeN+i] = nInfo[i].recovery;
		neuronPara[voltage*neuronSizeN+i] = nInfo[i].voltage;
		neuronPara[gAMPA*neuronSizeN+i] = nInfo[i].gAMPA;
		neuronPara[gNMDA_d*neuronSizeN+i] = nInfo[i].gNMDA_d;
		neuronPara[gGABAa*neuronSizeN+i] = nInfo[i].gGABAa;
		neuronPara[gGABAb_d*neuronSizeN+i] = nInfo[i].gGABAb_d;
	}
    swInfo.nSpikePoisAll=0;
	if(rank==0&&_MYID==0){
		
		printf("swInfo.MaxN%d\n",swInfo.SizeN);
		//printf("swInfo.MaxN%d",swInfo.MaxN);
		
	}

	return;	
}  

void freeSW(void *ptr){//????
	int nSpikeAll=0;
	int i;

	for(i=0;i<swInfo.SizeN;i++){
		nSpikeAll += nInfo[i].nSpikeCnt;
	}
	if(_MYID==0&&rank==0){
		printf("decayConduct: %.4lf ms\n", (double)(t1)*1000/(1.45e9));
		printf("dmatimes%d \n",dmatimes);
	}
	reply=0;
	athread_put(PE_MODE,&cdma,&dma[_MYID],sizeof(int),&reply,0,0);
	athread_put(PE_MODE,&cspike,&spike[_MYID],sizeof(int),&reply,0,0);
	athread_put(PE_MODE,&nSpikeAll,&numSpike[_MYID],sizeof(int),&reply,0,0);
	while(reply!=3);
	return;
}

void StateUpdate(void *ptr){
	int it;
	volatile time1,time2;

	{time1 = rpcc();}
	decayConduct(NULL);
	{time2 = rpcc();}
	t1 = time2-time1+t1;
	endST_mpi=0; topST_mpi=0; usedST_mpi=0;
	neuronUpdate_simd();
	sliceTime+=swInfo.Ndt;
	simTime++;
	assert(simTime==(sliceTime>>swInfo.Nop));
	offset += swInfo.Ndt;//ringBuffer offset????????
	if(offset>=lenRB) offset -= lenRB;
	SpikeDmaWrite_mpi(ptr);//mpi++++

	// if(_MYID==0&&rank==0){
	// 	for(it=0;it<6;it++){
	// 		printf("%d ",firingTable_mpi[it].nid);
	// 		printf("%d ",firingTable_mpi[it].time);
	// 	}
	// 	printf("\n");
	// }
	return;
}

static void decayConduct(void *ptr){

	int i=0;
	int SizeN=swInfo.SizeN;
	int sim_with_conductances=swInfo.sim_with_conductances;
	float dAMPA=swInfo.dAMPA;
	float dGABAa=swInfo.dGABAa;
	float dNMDA=swInfo.dNMDA;
	float dGABAb=swInfo.dGABAb;

	for(i=0; i<SizeN; i++) {
			if (sim_with_conductances) {
					nInfo[i].gAMPA*=dAMPA;
					neuronPara[gGABAa*neuronSizeN+i]*=dGABAa;
					neuronPara[gNMDA_d*neuronSizeN+i]*=dNMDA;//instantaneous rise
					neuronPara[gGABAb_d*neuronSizeN+i]*=dGABAb;//instantaneous rise					
			}
			else {
					nInfo[i].gAMPA=0.0f; //in CUBA current,sum up all wts
			}
	}
	return;
}
// static void decayConduct(void *ptr){

// 	int i=0;
//         for(i=0; i<swInfo.SizeN; i++) {

//                 if (swInfo.sim_with_conductances) {
//                         nInfo[i].gAMPA*=swInfo.dAMPA;
//                         nInfo[i].gGABAa*=swInfo.dGABAa;
//                         nInfo[i].gNMDA_d*=swInfo.dNMDA;//instantaneous rise
//                         nInfo[i].gGABAb_d*=swInfo.dGABAb;//instantaneous rise
//                 }
//                 else {
//                         nInfo[i].gAMPA=0.0f; //in CUBA current,sum up all wts
//                 }
//         }
// 	return;
// }

static int addSpikeToTable_mpi(int i){
	int spikeBufferFull = 0;
	if(i<swInfo.SizeN) {nInfo[i].nSpikeCnt++;}
	firingTable_mpi[endST_mpi].nid = i+swInfo.StartN;
	firingTable_mpi[endST_mpi].time= sliceTime;
	endST_mpi++; usedST_mpi++; 
	if(endST_mpi>=lenST_mpi) assert(0);//endST -= lenST;
	if(endST_mpi==topST_mpi) assert(0);
	if(usedST_mpi==lenST_mpi) assert(0);
	if(topST_mpi!=0) assert(0);
	return spikeBufferFull;
}

static void neuronUpdate_simd_wzc(){

	int i=0,j,it;
	floatv4 viNMDA, vtmpI;
	floatv4 vgNMDA, vgGABAb;
	int SizeN=swInfo.SizeN;
	floatv4 vgAMPA,vgGABAa;
	floatv4 vv;
	floatv4 vu;
	float tmpgNMDA_d[4],tmpgGABAb[4];
	float tmpgAMPA[4],tmpgGABAa[4];
	float tmpv[4],tmpu[4];
	floatv4 vh = swInfo.dt;
	floatv4 va = Izh_a;
	floatv4 vb = Izh_b;
	floatv4 v2_0=2.0,v6_0=6.0;
	floatv4 v0_5=0.5,v0_04=0.04,v5_0=5.0,v140_0=140.0;
	floatv4 vvolt = -60.0;
	floatv4 aa = 80.0,bb=60.0,cc=70.0,dd=90.0,ee=0.0,ff=1.;
	int sim_with_conductances=swInfo.sim_with_conductances;
	int Ndt = swInfo.Ndt;

	for(i=0;i<(SizeN/4)*4;i+=4) {
		
		simd_load(vgNMDA,&(neuronPara[gNMDA_d*neuronSizeN+i]));
		simd_load(vgGABAb,&(neuronPara[gGABAb_d*neuronSizeN+i]));
		simd_load(vgGABAa,&(neuronPara[gGABAa*neuronSizeN+i]));
		vgAMPA =simd_set_floatv4(nInfo[i].gAMPA,nInfo[i+1].gAMPA,nInfo[i+2].gAMPA,nInfo[i+3].gAMPA);
		simd_load(vv,&(neuronPara[voltage*neuronSizeN+i]));
		simd_load(vu,&(neuronPara[recovery*neuronSizeN+i]));

		for(it=0;it<Ndt;it++){
			if (sim_with_conductances) {
				viNMDA = (vvolt+aa)*(vvolt+aa)/(bb*bb);
				vtmpI=ee-(vgAMPA*(vvolt-ee)
					+vgNMDA*viNMDA/(ff+viNMDA)*(vvolt-ee)
					+vgGABAa*(vvolt+cc)
					+vgGABAb*(vvolt+dd));
			} else {
				vtmpI=vgAMPA;
			}
			
			/* 4th Runge-Kutta */
			//float k1,k2,k3,k4;
			//float l1,l2,l3,l4;			
			floatv4 vk1 = dvdtIzh_simd(vv, vu, vtmpI, vh);
			floatv4 vl1 = dudtIzh_simd(vv, vu, va, vb, vh);
			floatv4 vk2 = dvdtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, vtmpI, vh);
			floatv4 vl2 = dudtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, va, vb,vh);
			floatv4 vk3 = dvdtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, vtmpI, vh);
			floatv4 vl3 = dudtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, va, vb,vh);
			floatv4 vk4 = dvdtIzh_simd(vv+vk3, vu+vl3, vtmpI, vh);
			floatv4 vl4 = dudtIzh_simd(vv+vk3, vu+vl3, va, vb, vh);			
			vv += (vk1+v2_0*vk2+v2_0*vk3+vk4)/v6_0;
			vu += (vl1+v2_0*vl2+v2_0*vl3+vl4)/v6_0;
			simd_store(vu,&(tmpu[0]));
			simd_store(vv,&(tmpv[0]));

			for(j=0;j<4;j++){
				if(tmpv[j]<-90.0) {
					tmpv[j] = -90.0;
				}
				if (tmpv[j]>= 30.0) {
					tmpv[j] = Izh_c;
					tmpu[j]+= Izh_d;
					if(addSpikeToTable_simd_mpi(i+j,it)) assert(0);//????
				}
			}

			simd_load(vv,&(tmpv[0]));
			simd_load(vu,&(tmpu[0]));

			simd_store(vgAMPA,&(tmpgAMPA[0]));
			simd_store(vgNMDA,&(tmpgNMDA_d[0]));

			int dIndex=offset+it;////????????
			int addr = i*lenRB + dIndex;

			if (sim_with_conductances) {
				for(j=0;j<4;j++){
					int addr2 = addr+j*lenRB;
					tmpgAMPA[j] += ringBuffer[addr2];
					tmpgNMDA_d[j] += ringBuffer[addr2];
					ringBuffer[addr2] = 0.;
				}
			}
			simd_load(vgAMPA,&(tmpgAMPA[0]));
			simd_load(vgNMDA,&(tmpgNMDA_d[0]));
		} //end Ndt

		/****simd store******/

		simd_store(vv,&neuronPara[voltage*neuronSizeN+i]);
		simd_store(vu,&neuronPara[recovery*neuronSizeN+i]);
		simd_store(vgNMDA,&neuronPara[gNMDA_d*neuronSizeN+i]);
		simd_store(vgGABAa,&neuronPara[gGABAa*neuronSizeN+i]);
		simd_store(vgGABAb,&neuronPara[gGABAb_d*neuronSizeN+i]);

		for(j=0;j<4;j++){
			//neuronPara[gAMPA*neuronSizeN+i+j]=tmpgAMPA[j];
			nInfo[i+j].gAMPA=tmpgAMPA[j];
		}
		if(_MYID==0&&rank==0){
			//printf("%f %f %f %f\n",nInfo[i].gGABAb_d,nInfo[i].gGABAb_d,nInfo[i].gGABAb_d,nInfo[i].gGABAb_d);
			//printf("shuzu %f %f %f %f\n",nInfo[i].gAMPA,nInfo[i+1].gAMPA,nInfo[i+2].gAMPA,nInfo[i+3].gAMPA);
		}

	} // end SizeN
	int nr = SizeN-(SizeN/4)*4;
	if(nr>0){
		int i0=(SizeN/4)*4;
		float tmpa[4],tmpb[4];

		for(j=0;j<nr;j++){
			tmpgNMDA_d[j]=neuronPara[gNMDA_d*neuronSizeN+i0+j];
			tmpgGABAb[j]=neuronPara[gGABAb_d*neuronSizeN+i0+j];
			tmpgAMPA[j]=nInfo[i0+j].gAMPA;
			tmpgGABAa[j]=neuronPara[gGABAa*neuronSizeN+i0+j];
			tmpv[j]=neuronPara[voltage*neuronSizeN+i0+j];
			tmpu[j]=neuronPara[recovery*neuronSizeN+i0+j];
			tmpa[j]=Izh_a;
			tmpb[j]=Izh_b;
		}
		simd_load(vgNMDA,&(tmpgNMDA_d[0]));
		simd_load(vgGABAb,&(tmpgGABAb[0]));
		simd_load(vgAMPA,&(tmpgAMPA[0]));
		simd_load(vgGABAa,&(tmpgGABAa[0]));
		
		simd_load(vv,&(tmpv[0]));
		simd_load(vu,&(tmpu[0]));
		simd_load(va,&(tmpa[0]));
		simd_load(vb,&(tmpb[0]));

		floatv4 vh = swInfo.dt;

	for(it=0;it<Ndt;it++){
		if (sim_with_conductances) {
			floatv4 vvolt = -60.0;
			floatv4 aa = 80.0,bb=60.0,cc=70.0,dd=90.0,ee=0.0,ff=1.;
			viNMDA = (vvolt+aa)*(vvolt+aa)/(bb*bb);

			vtmpI=ee-(vgAMPA*(vvolt-ee)
				 +vgNMDA*viNMDA/(ff+viNMDA)*(vvolt-ee)
				 +vgGABAa*(vvolt+cc)
				 +vgGABAb*(vvolt+dd));
		} else {
			vtmpI=vgAMPA;
		}
		/* 4th Runge-Kutta */
		//float k1,k2,k3,k4;
		//float l1,l2,l3,l4;
		floatv4 v0_5=0.5,v0_04=0.04,v5_0=5.0,v140_0=140.0;
		floatv4 vk1 = dvdtIzh_simd(vv, vu, vtmpI, vh);
		floatv4 vl1 = dudtIzh_simd(vv, vu, va, vb, vh);
		floatv4 vk2 = dvdtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, vtmpI, vh);
		floatv4 vl2 = dudtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, va, vb,vh);
		floatv4 vk3 = dvdtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, vtmpI, vh);
		floatv4 vl3 = dudtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, va, vb,vh);
		floatv4 vk4 = dvdtIzh_simd(vv+vk3, vu+vl3, vtmpI, vh);
		floatv4 vl4 = dudtIzh_simd(vv+vk3, vu+vl3, va, vb, vh);
		floatv4 v2_0=2.0,v6_0=6.0;
		vv += (vk1+v2_0*vk2+v2_0*vk3+vk4)/v6_0;

		simd_store(vv,&(tmpv[0]));

		if (tmpv[0] < -90.0) tmpv[0] = -90.0;
		if (tmpv[1] < -90.0) tmpv[1] = -90.0;
		if (tmpv[2] < -90.0) tmpv[2] = -90.0;
		if (tmpv[3] < -90.0) tmpv[3] = -90.0;


		vu += (vl1+v2_0*vl2+v2_0*vl3+vl4)/v6_0;
		simd_store(vu,&(tmpu[0]));

		/*** findFiring ***/
		for(j=0;j<nr;j++){
			if (tmpv[j]>= 30.0) {
				tmpv[j] = Izh_c;
				tmpu[j]+= Izh_d;
				if(addSpikeToTable_simd_mpi(i0+j,it)) assert(0);//????
			}
		}

		simd_load(vv,&(tmpv[0]));
		simd_load(vu,&(tmpu[0]));

		/******* current set *******/
		int dIndex=offset+it;////????????
		assert(dIndex<lenRB);
		int addr = i0*lenRB + dIndex;

		simd_store(vgAMPA,&(tmpgAMPA[0]));
		simd_store(vgNMDA,&(tmpgNMDA_d[0]));

		if (sim_with_conductances) {
			for(j=0;j<nr;j++){
				int addr2 = addr+j*lenRB;
				tmpgAMPA[j] += ringBuffer[addr2];
				tmpgNMDA_d[j] += ringBuffer[addr2];
				ringBuffer[addr2] = 0.;
			}
		}
		simd_load(vgAMPA,&(tmpgAMPA[0]));
		simd_load(vgNMDA,&(tmpgNMDA_d[0]));
	} 
	/****simd store******/
		simd_store(vgGABAa,&(tmpgGABAa[0]));
		simd_store(vgGABAb,&(tmpgGABAb[0]));

		for(j=0;j<nr;j++){
			neuronPara[voltage*neuronSizeN+i0+j]=tmpv[j];
			neuronPara[recovery*neuronSizeN+i0+j]=tmpu[j];
			nInfo[i0+j].gAMPA=tmpgAMPA[j];
			neuronPara[gNMDA_d*neuronSizeN+i0+j]=tmpgNMDA_d[j];
			neuronPara[gGABAa*neuronSizeN+i0+j]=tmpgGABAa[j];
			neuronPara[gGABAb_d*neuronSizeN+i0+j]=tmpgGABAb[j];
		}
	}
}
#if 1
static void neuronUpdate_simd(){
	int i=0,j,it;
	floatv4 viNMDA, vtmpI;
	floatv4 vgNMDA, vgGABAb;
	int SizeN=swInfo.SizeN;
	floatv4 vgAMPA,vgGABAa;
	floatv4 vv;
	floatv4 vu;
	float tmpgNMDA_d[4],tmpgGABAb[4];
	float tmpgAMPA[4],tmpgGABAa[4];
	float tmpv[4],tmpu[4];
	floatv4 vh = swInfo.dt;
	floatv4 va = Izh_a;
	floatv4 vb = Izh_b;
	floatv4 v2_0=2.0,v6_0=6.0;
	floatv4 v0_5=0.5,v0_04=0.04,v5_0=5.0,v140_0=140.0;
	floatv4 vvolt = -60.0;
	floatv4 aa = 80.0,bb=60.0,cc=70.0,dd=90.0,ee=0.0,ff=1.;
	int sim_with_conductances=swInfo.sim_with_conductances;
	int Ndt = swInfo.Ndt;
	float *pvgAMPA,*pvgNMDA;
	pvgAMPA = &vgAMPA;
	pvgNMDA = &vgNMDA;

	for(i=0;i<(SizeN/4)*4;i+=4) {
		
		simd_load(vgNMDA,&(neuronPara[gNMDA_d*neuronSizeN+i]));
		simd_load(vgGABAb,&(neuronPara[gGABAb_d*neuronSizeN+i]));
		simd_load(vgGABAa,&(neuronPara[gGABAa*neuronSizeN+i]));
		vgAMPA =simd_set_floatv4(nInfo[i].gAMPA,nInfo[i+1].gAMPA,nInfo[i+2].gAMPA,nInfo[i+3].gAMPA);
		simd_load(vv,&(neuronPara[voltage*neuronSizeN+i]));
		simd_load(vu,&(neuronPara[recovery*neuronSizeN+i]));

		for(it=0;it<Ndt;it++){
			if (sim_with_conductances) {
				viNMDA = (vvolt+aa)*(vvolt+aa)/(bb*bb);
				vtmpI=ee-(vgAMPA*(vvolt-ee)
					+vgNMDA*viNMDA/(ff+viNMDA)*(vvolt-ee)
					+vgGABAa*(vvolt+cc)
					+vgGABAb*(vvolt+dd));
			} else {
				vtmpI=vgAMPA;
			}
			
			/* 4th Runge-Kutta */
			//float k1,k2,k3,k4;
			//float l1,l2,l3,l4;			
			floatv4 vk1 = dvdtIzh_simd(vv, vu, vtmpI, vh);
			floatv4 vl1 = dudtIzh_simd(vv, vu, va, vb, vh);
			floatv4 vk2 = dvdtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, vtmpI, vh);
			floatv4 vl2 = dudtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, va, vb,vh);
			floatv4 vk3 = dvdtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, vtmpI, vh);
			floatv4 vl3 = dudtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, va, vb,vh);
			floatv4 vk4 = dvdtIzh_simd(vv+vk3, vu+vl3, vtmpI, vh);
			floatv4 vl4 = dudtIzh_simd(vv+vk3, vu+vl3, va, vb, vh);			
			vv += (vk1+v2_0*vk2+v2_0*vk3+vk4)/v6_0;
			vu += (vl1+v2_0*vl2+v2_0*vl3+vl4)/v6_0;

			float *pvv,*pvu;
			pvv = &vv;
			pvu = &vu;

			for(j=0;j<4;j++){
				if(pvv[j]<-90.0) {
					pvv[j] = -90.0;
				}
				if (pvv[j]>= 30.0) {
					pvv[j] = Izh_c;
					pvu[j]+= Izh_d;
					if(addSpikeToTable_simd_mpi(i+j,it)) assert(0);//????
				}
			}

			int dIndex=offset+it;////????????
			int addr = i*lenRB + dIndex;

			if (sim_with_conductances) {
				for(j=0;j<4;j++){
					int addr2 = addr+j*lenRB;
					pvgAMPA[j] += ringBuffer[addr2];
					pvgNMDA[j] += ringBuffer[addr2];
					ringBuffer[addr2] = 0.;
				}
			}
		} //end Ndt

		/****simd store******/

		simd_store(vv,&neuronPara[voltage*neuronSizeN+i]);
		simd_store(vu,&neuronPara[recovery*neuronSizeN+i]);
		simd_store(vgNMDA,&neuronPara[gNMDA_d*neuronSizeN+i]);
		simd_store(vgGABAa,&neuronPara[gGABAa*neuronSizeN+i]);
		simd_store(vgGABAb,&neuronPara[gGABAb_d*neuronSizeN+i]);

		for(j=0;j<4;j++){
			//neuronPara[gAMPA*neuronSizeN+i+j]=tmpgAMPA[j];
			nInfo[i+j].gAMPA=pvgAMPA[j];
		}
	} // end SizeN

	int nr = SizeN-(SizeN/4)*4;
	if(nr>0){
		int i0=(SizeN/4)*4;
		float tmpa[4],tmpb[4];

		for(j=0;j<nr;j++){
			tmpgNMDA_d[j]=neuronPara[gNMDA_d*neuronSizeN+i0+j];
			tmpgGABAb[j]=neuronPara[gGABAb_d*neuronSizeN+i0+j];
			tmpgAMPA[j]=nInfo[i0+j].gAMPA;
			tmpgGABAa[j]=neuronPara[gGABAa*neuronSizeN+i0+j];
			tmpv[j]=neuronPara[voltage*neuronSizeN+i0+j];
			tmpu[j]=neuronPara[recovery*neuronSizeN+i0+j];
			tmpa[j]=Izh_a;
			tmpb[j]=Izh_b;
		}
		simd_load(vgNMDA,&(tmpgNMDA_d[0]));
		simd_load(vgGABAb,&(tmpgGABAb[0]));
		simd_load(vgAMPA,&(tmpgAMPA[0]));
		simd_load(vgGABAa,&(tmpgGABAa[0]));
		
		simd_load(vv,&(tmpv[0]));
		simd_load(vu,&(tmpu[0]));
		simd_load(va,&(tmpa[0]));
		simd_load(vb,&(tmpb[0]));

		floatv4 vh = swInfo.dt;

		for(it=0;it<Ndt;it++){
			if (sim_with_conductances) {
				floatv4 vvolt = -60.0;
				floatv4 aa = 80.0,bb=60.0,cc=70.0,dd=90.0,ee=0.0,ff=1.;
				viNMDA = (vvolt+aa)*(vvolt+aa)/(bb*bb);


				vtmpI=ee-(vgAMPA*(vvolt-ee)
					+vgNMDA*viNMDA/(ff+viNMDA)*(vvolt-ee)
					+vgGABAa*(vvolt+cc)
					+vgGABAb*(vvolt+dd));
			} else {
				vtmpI=vgAMPA;
			}
			/* 4th Runge-Kutta */
			//float k1,k2,k3,k4;
			//float l1,l2,l3,l4;
			floatv4 v0_5=0.5,v0_04=0.04,v5_0=5.0,v140_0=140.0;
			floatv4 vk1 = dvdtIzh_simd(vv, vu, vtmpI, vh);
			floatv4 vl1 = dudtIzh_simd(vv, vu, va, vb, vh);
			floatv4 vk2 = dvdtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, vtmpI, vh);
			floatv4 vl2 = dudtIzh_simd(vv+v0_5*vk1, vu+v0_5*vl1, va, vb,vh);
			floatv4 vk3 = dvdtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, vtmpI, vh);
			floatv4 vl3 = dudtIzh_simd(vv+v0_5*vk2, vu+v0_5*vl2, va, vb,vh);
			floatv4 vk4 = dvdtIzh_simd(vv+vk3, vu+vl3, vtmpI, vh);
			floatv4 vl4 = dudtIzh_simd(vv+vk3, vu+vl3, va, vb, vh);
			floatv4 v2_0=2.0,v6_0=6.0;
			vv += (vk1+v2_0*vk2+v2_0*vk3+vk4)/v6_0;

			simd_store(vv,&(tmpv[0]));

			if (tmpv[0] < -90.0) tmpv[0] = -90.0;
			if (tmpv[1] < -90.0) tmpv[1] = -90.0;
			if (tmpv[2] < -90.0) tmpv[2] = -90.0;
			if (tmpv[3] < -90.0) tmpv[3] = -90.0;

			vu += (vl1+v2_0*vl2+v2_0*vl3+vl4)/v6_0;
			simd_store(vu,&(tmpu[0]));

			/*** findFiring ***/
			for(j=0;j<nr;j++){
				if (tmpv[j]>= 30.0) {
					tmpv[j] = Izh_c;
					tmpu[j]+= Izh_d;
					if(addSpikeToTable_simd_mpi(i0+j,it)) assert(0);//????
				}
			}
			simd_load(vv,&(tmpv[0]));
			simd_load(vu,&(tmpu[0]));
			/******* current set *******/
			int dIndex=offset+it;////????????
			assert(dIndex<lenRB);
			int addr = i0*lenRB + dIndex;

			simd_store(vgAMPA,&(tmpgAMPA[0]));
			simd_store(vgNMDA,&(tmpgNMDA_d[0]));

			if (sim_with_conductances) {
				for(j=0;j<nr;j++){
					int addr2 = addr+j*lenRB;
					tmpgAMPA[j] += ringBuffer[addr2];
					tmpgNMDA_d[j] += ringBuffer[addr2];
					ringBuffer[addr2] = 0.;
				}
			}
			simd_load(vgAMPA,&(tmpgAMPA[0]));
			simd_load(vgNMDA,&(tmpgNMDA_d[0]));
		} 
	/****simd store******/
		simd_store(vgGABAa,&(tmpgGABAa[0]));
		simd_store(vgGABAb,&(tmpgGABAb[0]));

		for(j=0;j<nr;j++){
			neuronPara[voltage*neuronSizeN+i0+j]=tmpv[j];
			neuronPara[recovery*neuronSizeN+i0+j]=tmpu[j];
			nInfo[i0+j].gAMPA=tmpgAMPA[j];
			neuronPara[gNMDA_d*neuronSizeN+i0+j]=tmpgNMDA_d[j];
			neuronPara[gGABAa*neuronSizeN+i0+j]=tmpgGABAa[j];
			neuronPara[gGABAb_d*neuronSizeN+i0+j]=tmpgGABAb[j];
		}
	}
}
#endif

static int addSpikeToTable_simd_mpi(int i,int it) {
	int spikeBufferFull = 0;
	if(i<swInfo.SizeN) {nInfo[i].nSpikeCnt++;}//mpi++++
	firingTable_mpi[endST_mpi].nid = i+swInfo.StartN;
	if (firingTable_mpi[endST_mpi].nid==0xffff) firingTable_mpi[endST_mpi].nid = 0;
	firingTable_mpi[endST_mpi].time= it+sliceTime;//????????
	endST_mpi++; usedST_mpi++;

	return spikeBufferFull;
}

//write firingTable_mpi[] from slave to host memory

static int SpikeDmaWrite_mpi(ptr){//mpi++++

	int i,core_number,all_number=0;
	//gather slaves spike numbers
	intv8 numCol = 0;
	intv8 numRow = 0;
	int col = (_MYID)%8;
	int row = (_MYID)/8;
	intv8 _v = 0;
	((int*)(&numCol))[col] = (int)endST_mpi;
	
	if(col==1||col==3||col==5||col==7){
		REG_PUTR(numCol,col-1);
	}
	if(col==0||col==2||col==4||col==6){
		REG_GETR(_v);numCol |= _v;
	}
	if(col==2||col==6){
		REG_PUTR(numCol,col-2);
	}
	if(col==0||col==4){
		REG_GETR(_v);numCol |= _v;
	}
	if(col==4){
		REG_PUTR(numCol,col-4);
	}
	if(col==0){
		REG_GETR(_v);numCol |= _v;
		for(i=0;i<8;i++)
			((int*)(&numRow))[row] += ((int*)(&numCol))[i];
	}

	if(row==1||row==3||row==5||row==7){
		REG_PUTC(numRow,row-1);
	}
	if(row==0||row==2||row==4||row==6){
		REG_GETC(_v);numRow |= _v;
	}
	if(row==2||row==6){
		REG_PUTC(numRow,row-2);
	}
	if(row==0||row==4){
		REG_GETC(_v);numRow |= _v;
	}
	if(row==4){
		REG_PUTC(numRow,row-4);
	}
	if(row==0){
		REG_GETC(_v);numRow |= _v;
	}	

	if (row==0){REG_PUTC(numRow,8);}
	else {REG_GETC(numRow);}		
	
	if (col==0){REG_PUTR(numCol,8);REG_PUTR(numRow,8);}
	else {REG_GETR(numCol);REG_GETR(numRow);}
    
	//dma write
	long addr = 0,reply = 0;
	for(i=0;i<col;i++) addr += ((int*)(&numCol))[i];
	for(i=0;i<row;i++) addr += ((int*)(&numRow))[i];

	if(endST_mpi){
		athread_put(PE_MODE,&(firingTable_mpi[0]),&(swInfo.firingTableHost[addr]),sizeof(spikeTime_t)*endST_mpi,&reply,0,0);
		dmaWait(&reply,1);
	}
	if(_MYID==0){
		NSgroup_slave = 0;
		for(i=0;i<8;i++) NSgroup_slave += ((int*)(&numRow))[i];
		reply = 0;
		athread_put(PE_MODE,&(NSgroup_slave),&(NS_group),sizeof(int),&reply,0,0);//发送脉冲数量
		dmaWait(&reply,1);
	}
	return 0;
}

//read firingTableAll_mpi[] from host memory to slave
static int SpikeDmaRead_mpi(ptr){//mpi++++
	//get data for firingTableAll[]
	if(_MYID==0){
		reply = 0;
		numST_mpi[simTime%swInfo.Ndelay] = 0xfffffff;
		athread_get(PE_MODE,
			&(NSall),
			&(numST_mpi[simTime%swInfo.Ndelay]),
			sizeof(int),
			&reply,0,0,0);	
		while(numST_mpi[simTime%swInfo.Ndelay]==0xfffffff);
	}
	
	intv8 _v;
	((int*)(&_v))[0] = numST_mpi[simTime%swInfo.Ndelay];
	_v = put_get_intv8(_v,0);
	numST_mpi[simTime%swInfo.Ndelay]=((int*)(&_v))[0];
	return 0;
}


void SpikeDeliver(void *ptr){
	SpikeDmaRead_mpi(ptr);//mpi++++
	CurrentUpdateWzc(ptr);
	//CurrentUpdate_mpi(ptr);
	float wt=0.00085;
	float nspike;
	nspike = currentfactor;
	InputCurrent(wt,nspike);
	return;
}
static void InputCurrent(float wt, float nspike)
{
	int dIndex = offset+swInfo.Ndt/2;
	if (dIndex>=lenRB){dIndex-=lenRB;}
	float change = wt*nspike;
	int i;
	for(i=0;i<swInfo.SizeN;i++){
		ringBuffer[i*lenRB+dIndex] += change;
	}
}

static void put_get_syn(synInfo_t *sInfoLc){

	intv8 *_v=(intv8*)sInfoLc;
	int len = swInfo.MaxN/4;
	int ilc = COL(_MYID);
	int i,j,offset;

	if(_MYID&0x01){
		intv8 *_vsrc = _v-len;
		int dst=ilc-1;
			REG_PUTR(_vsrc[0],dst);
			REG_PUTR(_vsrc[1],dst);
			REG_PUTR(_vsrc[2],dst);
			REG_PUTR(_vsrc[3],dst);
			REG_PUTR(_vsrc[4],dst);
			REG_GETR(_v[0]);
			REG_GETR(_v[1]);
			REG_GETR(_v[2]);
			REG_GETR(_v[3]);
			REG_GETR(_v[4]);

	}
	else{
		intv8 *_vsrc = _v+len;
		int dst=ilc+1;
			REG_GETR(_v[0]);
			REG_GETR(_v[1]);
			REG_GETR(_v[2]);
			REG_GETR(_v[3]);
			REG_GETR(_v[4]);
			REG_PUTR(_vsrc[0],dst);
			REG_PUTR(_vsrc[1],dst);
			REG_PUTR(_vsrc[2],dst);
			REG_PUTR(_vsrc[3],dst);
			REG_PUTR(_vsrc[4],dst);
	
	}
	return;
}

void DealSynaptic(int time,synInfo_t *sInfoLc);
void CurrentUpdateWzc(void *ptr);
int ReadFiringTable(int i,int spikeNumber);
void ReadSynaptic(int DMASynLenth,int pulseID,synInfo_t *sInfoLc);

void CurrentUpdateWzc(void *ptr){
	int i,j,k;
	int spikeNumber = numST_mpi[0];
	int DMATableLenth;
	int DMASynLenth = 10000/nproc; //脉冲数量10000

	int neuronSynNumber = 10000;//80000神经元
	synInfo_t *sInfoLc=&sInfo[0];
	int pulseID;



	/*大部分spikenumber为0，个别为1就直接过滤了*/
	if(spikeNumber > 5){
		/*多一次循环*/			
		for(i = -lenST_mpi; i < spikeNumber; i = i + lenST_mpi){
			DMATableLenth = ReadFiringTable(i, spikeNumber);//分段读取firingtable		
			for(j = 0; j < DMATableLenth; ++j){

				int findNumber = 0;
				pulseID = firingTable_mpi[j].nid;
				while(findNumber<4){
					if(j+1<DMATableLenth && firingTable_mpi[j+1].nid==pulseID+1){
						findNumber++;
					}else{
						break;
					}
				}
				j=j+findNumber;

				int pulseID = firingTable_mpi[j].nid;

				/*一开始想法是id在64从核各自范围内，由各自从核处理，测了一下id值，id只在1-500左右
				*所以理想化操作
				*/
			}
		}		
	}

	if(simTime==100||simTime==200||simTime==300||simTime==400||simTime==500||simTime==600||simTime==700||simTime==800||simTime==900||simTime==20){
		for(k = 0; k < neuronSynNumber; k++){
			ReadSynaptic(DMASynLenth, k, sInfoLc);//每个对应的神经元读取10000/8个突触
			DealSynaptic(simTime, sInfoLc);//对每个对应的神经元读取10000/8个突触进行操作
		}
	}
	return ;
}

int ReadFiringTable(int i,int spikeNumber){//读取10000/8个table
	int DMATableLenth = lenST_mpi;
	if((i + lenST_mpi) > spikeNumber){
		DMATableLenth = spikeNumber - i;
	}
	reply = 0;
	athread_get(PE_MODE,
					&(swInfo.firingTableAll[0]),
					&(firingTable_mpi[0]),
					sizeof(spikeTime_t) * DMATableLenth,
					&reply,0,0,0);
	dmaWait(&reply,1);
	return DMATableLenth;
}

void DealSynaptic(int time ,synInfo_t *sInfoLc){//抄的generatePostSpike
	int m;
	int Ndelay = swInfo.Ndelay;
	short occurTime = time>>swInfo.Nop;
	short iD = simTime - occurTime - 1;
	for(m=0; m<20; m++){
		unsigned int post_i = sInfoLc[m].postId;
		unsigned int dIndex = time + sInfoLc[m].dl - sliceTime + offset;
		dIndex &= (lenRB - 1);
		float change = sInfoLc[m].wt;
		int i = post_i&0xf;
		//ringBuffer[i*lenRB+dIndex] += change;
	}
	return;
}

void ReadSynaptic(int DMASynLenth, int pulseID, synInfo_t *sInfoLc){//读突触
	dmatimes++;
	int addr = DMASynLenth * pulseID + _MYID*20;//ndma 20
	reply = 0;
	athread_get(PE_MODE,
				&swInfo.sInfoHost[addr],
				sInfoLc,
				sizeof(spikeTime_t) * 20,
				&reply,0,0,0);
	dmaWait(&reply,1);
	return ;
}


static void CurrentUpdate_mpi(void *ptr){//????
	int i,nid,srcId,iSpike,ibreak;
	int j;
	int Ndma = swInfo.Ndma;

	intv8 _v[8];
	int idelay, iblock,nblock;
	for(idelay=0; idelay<swInfo.Ndelay; idelay++) {//swInfo.Ndelay==1
		iSpike=topST;
		ibreak=0;
		int ivarray=0;
		int tmp1 = 0;
		while(1){
			if(_MYID==0){
				int nvarray = lenST_mpi/64;//lenst_mpi=size+64
				int length = lenST_mpi/64*64;
				if (ivarray%nvarray==0){
					int least = numST_mpi[idelay]-ivarray*64;
					iSpike=0;
					endST_mpi = MIN(least,length);

					if(endST_mpi>0){
						reply = 0;
						athread_get(PE_MODE,
										&(swInfo.firingTableAll[0]),
										&(firingTable_mpi[0]),
										sizeof(spikeTime_t)*endST_mpi,
										&reply,0,0,0);
						dmaWait(&reply,1);

						if(endST_mpi>5&&rank==0&&tmp1==0){
							//printf("%d %d %d %d %d\n",firingTable_mpi[0].nid,firingTable_mpi[1].nid,firingTable_mpi[2].nid,firingTable_mpi[3].nid,firingTable_mpi[4].nid);
							tmp1++;
						}
					}
					if (endST_mpi<0){fprintf(stderr,"warn::more than number\n");}
				}
				for(j=0;j<8;j++){
					for(i=0;i<8;i++){
						if(iSpike==endST_mpi) {//if 循环没有用
							int ii;
							for(ii=i;ii<8;ii++){
								((spikeTime_t*)(&_v[j]))[ii].nid=0xffff;
							}
							break;//????
						}
						((spikeTime_t*)(&_v[j]))[i]=firingTable_mpi[iSpike];
						iSpike++;
					}
				}
			}
			ivarray++;
			for(j=0;j<8;j++){			
				_v[j] = put_get_intv8(_v[j],0);
				if(((spikeTime_t*)(&_v[j]))[7].nid==0xffff) break;//直接广播
			}

			spikeTime_t st,st0;
			long tm0,tm1,tm2,tm3;

			st=((spikeTime_t*)(&_v[0]))[0];
			if(st.nid!=0xffff){
				rpl[0]=0;
				{tm0=rpcc();{
					dma_set_reply(&dma_get_syn,&rpl[0]);
					synInfo_t *sInfoLc=&sInfo[0];
					syndma(st,sInfoLc);
				}tm1=rpcc();}
				cdma+=tm1-tm0;
			}
			
			for(i=1;i<64;i++){
				st0=st;
				st=((spikeTime_t*)(&_v[0]))[i];
				if(st.nid==0xffff) {ibreak=1;break;}
				j=i&(0x07);
				rpl[j]=0;
        		dma_set_reply(&dma_get_syn,&rpl[j]);
				synInfo_t *sInfoLc=&sInfo[j*Ndma];

				{tm0=rpcc();{
					syndma(st,sInfoLc);
					//dmatimes++;
					j=(i-1)&(0x07);
					dma_wait(&rpl[j],1);
				}tm1=rpcc();}
				sInfoLc=&sInfo[(j)*Ndma];
				{tm2=rpcc();{
					generatePostSpike_simd(st0,sInfoLc);
				}tm3=rpcc();}
				cdma+=tm1-tm0;
				cspike+=tm3-tm2;
			}

			st=((spikeTime_t*)(&_v)[0])[i-1];
			if(st.nid!=0xffff) {
				j=(i-1)&(0x07);
				{tm0=rpcc();{
					dma_wait(&rpl[j],1);
				}tm1=rpcc();}
				cdma+=tm1-tm0;
				synInfo_t *sInfoLc=&sInfo[j*swInfo.Ndma];
				{tm2=rpcc();{
					generatePostSpike_simd(st,sInfoLc);
				}tm3=rpcc();}
				cspike+=tm3-tm2;
			}
			if(ibreak) break;
		}		
	}
	/****chenge topST****/
	short imv=simTime%swInfo.Ndelay;//IMV=0
	topST += numST[imv];
  	if(topST>=lenST){topST-=lenST;}
	usedST -= numST[imv];
	numST[imv]=0;
}

static void generatePostSpike(spikeTime_t st,synInfo_t *sInfoLc) 
{
	int is;
	int MaxN=swInfo.MaxN;
	int Ndma=swInfo.Ndma;
	//printf("%ddam \n",dma);
	int Ndelay=swInfo.Ndelay; 
	short occurTime=st.time>>swInfo.Nop;
	short iD = simTime-occurTime-1;
	if(!(iD>=0&&iD<Ndelay))iD = 0;
	for (is=0;is<Ndma;is++){
		unsigned int post_i = sInfoLc[is].postId;
		if(post_i==0xffff) break;
		unsigned int dIndex = st.time+sInfoLc[is].dl-sliceTime+offset;
		dIndex &= (lenRB-1);
		float change = sInfoLc[is].wt;
		int i = post_i&0xf;
		ringBuffer[i*lenRB+dIndex] += change;
	}
	return;
}

static void syndma(spikeTime_t st,synInfo_t *sInfoLc) {
	int pre = st.nid;
	int MaxN = swInfo.MaxN;
	int Ndelay = swInfo.Ndelay;
	int preN = swInfo.preN;
	if(pre > preN && pre < 0) {
		pre = 0;
	}
	short occurTime = st.time>>swInfo.Nop;
	short iD = simTime-occurTime-1;
	if(iD < 0 && iD >= Ndelay){
		iD = 0;
	}
	unsigned int addr = pre*Ndelay*NTh*MaxN+iD*NTh*MaxN+_MYID*MaxN;
	dma(dma_get_syn,&swInfo.sInfoHost[addr],sInfoLc);
	
	return;
	
}

static void syndma2(spikeTime_t st,synInfo_t *sInfoLc) {
	int pre_i=st.nid;
	int is;
	int MaxN=swInfo.MaxN;
	int Ndma=swInfo.Ndma;
	int Ndelay=swInfo.Ndelay;

	short occurTime=st.time>>swInfo.Nop;
	short iD = simTime-occurTime-1;
	if(!(iD>=0&&iD<Ndelay))iD=0;
	unsigned int addr = pre_i*Ndelay*NTh*MaxN+iD*NTh*MaxN+(_MYID/2)*2*MaxN;
	dma(dma_get_syn,&swInfo.sInfoHost[addr],sInfoLc);
	return;
}

#if 1
static void generatePostSpike_simd(spikeTime_t st,synInfo_t *sInfoLc) {
	int pre_i=st.nid;
	int is,j;
	int MaxN=swInfo.MaxN;
	int Ndma=swInfo.Ndma;
	int Ndelay=swInfo.Ndelay;

	short occurTime=st.time>>swInfo.Nop;
	short iD = simTime-occurTime-1;
	if(!(iD>=0&&iD<Ndelay))iD=0;

	intv8 vtime=simd_set_intv8((int)st.time,(int)st.time,(int)st.time,(int)st.time,(int)st.time,(int)st.time,(int)st.time,(int)st.time);
	intv8 vsliceTime=(intv8)sliceTime;
	intv8 voffset=(intv8)offset;
	intv8 vStartN=(intv8)swInfo.StartN;
	intv8 vlenRB=(intv8)lenRB;
	for (is=0;is<(Ndma/8*8);is+=8){
		if(sInfoLc[is+7].postId==0xffff)break;
		intv8 vdl,vpid;
		vdl=simd_set_intv8((int)(sInfoLc[is].dl),(int)(sInfoLc[is+1].dl),(int)(sInfoLc[is+2].dl),(int)(sInfoLc[is+3].dl),(int)(sInfoLc[is+4].dl),(int)(sInfoLc[is+5].dl),(int)(sInfoLc[is+6].dl),(int)(sInfoLc[is+7].dl));
		vpid=simd_set_intv8((int)(sInfoLc[is].postId),(int)(sInfoLc[is+1].postId),(int)(sInfoLc[is+2].postId),(int)(sInfoLc[is+3].postId),(int)(sInfoLc[is+4].postId),(int)(sInfoLc[is+5].postId),(int)(sInfoLc[is+6].postId),(int)(sInfoLc[is+7].postId));
		intv8 vdIndex = vtime+vdl-vsliceTime+voffset;
		intv8 vlen = lenRB-1;
		vdIndex = vdIndex & vlen;
		intv8 vff = 0xf;
		intv8 vi = vpid&vff;
		intv8 vNop = (intv8)(swInfo.Nop+1);
		int op = swInfo.Nop+1;
		intv8 vaddr=vi<<4;
		vaddr += vdIndex;
		ringBuffer[((int*)(&vaddr))[0]] += sInfoLc[is].wt;
		ringBuffer[((int*)(&vaddr))[1]] += sInfoLc[is+1].wt;
		ringBuffer[((int*)(&vaddr))[2]] += sInfoLc[is+2].wt;
		ringBuffer[((int*)(&vaddr))[3]] += sInfoLc[is+3].wt;
		ringBuffer[((int*)(&vaddr))[4]] += sInfoLc[is+4].wt;
		ringBuffer[((int*)(&vaddr))[5]] += sInfoLc[is+5].wt;
		ringBuffer[((int*)(&vaddr))[6]] += sInfoLc[is+6].wt;
		ringBuffer[((int*)(&vaddr))[7]] += sInfoLc[is+7].wt;
	}
/*************residule syn***********************/
#if 1
	for (;is<Ndma;is++){
		unsigned int post_i = sInfoLc[is].postId;
		if(post_i==0xffff) break;
		int dIndex = st.time+sInfoLc[is].dl-sliceTime+offset;
		dIndex &= (lenRB-1);
		float change = sInfoLc[is].wt;
		int i = post_i&0xf;
		ringBuffer[i*lenRB+dIndex] += change;
	}
#endif

	return;
}
#endif
