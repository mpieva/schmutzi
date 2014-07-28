/*
 * mtCont
 * Date: Mar-26-2014 
 * Author : Gabriel Renaud gabriel.reno [at sign here ] gmail.com
 *
 */




#include <api/BamConstants.h>
#include <api/BamMultiReader.h>
#include <utils/bamtools_fasta.h>
#include <utils/bamtools_options.h>
#include <utils/bamtools_pileup_engine.h>
#include <utils/bamtools_utilities.h>
#include <gzstream.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <limits>
#include <queue>

#include "utils.h"
#include "miscfunc.h"


using namespace BamTools;
using namespace std;

#define MAXCOV 5000
#define MINPRIOR 0.001

// #define DEBUG1
// #define DEBUG2

#define checkResults(string, val) {             \
 if (val) {                                     \
     cerr<<"Failed with "<<val<<" at "<<string<<endl;	\
   exit(1);                                     \
 }                                              \
}
 


char   offsetQual=33;
double likeMatch[64];
double likeMismatch[64];
double likeMatchMQ[64][64];
double likeMismatchMQ[64][64];

double likeMatchProb[64];
double likeMismatchProb[64];
double likeMatchProbMQ[64][64];
double likeMismatchProbMQ[64][64];

probSubstition illuminaErrorsProb;
vector<probSubstition> sub5p;
vector<probSubstition> sub3p;
probSubstition defaultSubMatch;

double probMapping[64];
double probMismapping[64];

string dnaAlphabet="ACGT";

#define MIN(a,b) (((a)<(b))?(a):(b))


queue<string>  queueFilesToprocess;
vector< vector<contaminationEstimate> > outputToPrint;

pthread_mutex_t  mutexQueue   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  mutexCounter = PTHREAD_MUTEX_INITIALIZER;


//GLOBALLY accessed
int sizeGenome=0;
vector<positionInformation> infoPPos;
map<int, PHREDgeno> pos2phredgeno;
map<unsigned int, int> threadID2Rank;

bool             doneReading;


template <typename T>
inline string arrayToStringInt(const T toPrint[] ,const int size,const string separator=","){
    if(size == 0){
    	return "";
    }
    string toReturn="";
    for(int i=0;i<(size-1);i++){
    	toReturn+=(stringify(int(toPrint[i]))+separator);
    }
    toReturn+=(stringify(int(toPrint[ size -1 ])));
    return toReturn;
}







void *mainContaminationThread(void * argc){


    int   rc;
    // int   stackIndex;
    string freqFileNameToUse;

    
 checkqueue:    
    // stackIndex=-1;
    //check stack
    bool foundData=false;
    rc = pthread_mutex_lock(&mutexQueue);
    checkResults("pthread_mutex_lock()\n", rc);

    threadID2Rank[(unsigned int)pthread_self()]  = threadID2Rank.size()+1;

    cerr<<"Thread #"<<(unsigned int)pthread_self() <<" started "<<endl;
    cerr<<"Thread #"<<threadID2Rank[(unsigned int)pthread_self()] <<" started "<<endl;

	
    // cout<<"Thread "<<(unsigned int)pthread_self()<<" taking mutex queue "<<endl;
    if(!queueFilesToprocess.empty()){    
 	foundData=true;
 	freqFileNameToUse = queueFilesToprocess.front();
 	queueFilesToprocess.pop();
 	cerr<<"Thread #"<<threadID2Rank[(unsigned int)pthread_self()]<<" is reading "<<freqFileNameToUse<<endl;

    }

    
    //release stack
    rc = pthread_mutex_unlock(&mutexQueue);
    checkResults("pthread_mutex_unlock()\n", rc);


    if(!foundData){
 	if(doneReading){
 	    cerr<<"Thread #"<<threadID2Rank[(unsigned int)pthread_self()]<<" is done"<<endl;
 	    return NULL;	
 	}else{
 	    sleep(1);
 	    goto checkqueue;	   
 	}
    }


    //////////////////////////////////////////////////////////////
    //                BEGIN COMPUTATION                         //
    //////////////////////////////////////////////////////////////



    vector<contaminationEstimate> toAddToLog;
    double contaminationRate=0.0;

    map<int, alleleFrequency> freqFromFile;
    readMTAlleleFreq(freqFileNameToUse,	freqFromFile);


    //////////////////////
    //pre-computations ///
    //////////////////////

    cerr<<"Thread #"<<threadID2Rank[(unsigned int)pthread_self()] <<" started  pre-computations"<<endl;

    // map<int, diNucleotideProb> priorDiNucVec;
    // map<int, vector<diNucleotideProb> > probConsVec;
    // map<int, vector<diNucleotideProb> > probContVec;
    vector<         diNucleotideProb   >  priorDiNucVec;
    vector< vector< diNucleotideProb > >  probConsVec;
    vector< vector< diNucleotideProb > >  probContVec;
    
    priorDiNucVec.reserve(sizeGenome+1);
    probContVec.reserve(sizeGenome+1);
    probConsVec.reserve(sizeGenome+1);



    for(int i=0;i<sizeGenome;i++){
	//for(int i=261;i<=262;i++){


	 // cout<<"pre i"<<i<<"\t"<<infoPPos[i].posAlign<<endl;
	// cout<<infoPPos[i].cov<<endl;
	// cout<<(pos2phredgeno.find( infoPPos[i].posAlign ) == pos2phredgeno.end())<<endl;
	if( (infoPPos[i].cov == 0) || //no coverage
	    (pos2phredgeno.find( infoPPos[i].posAlign ) == pos2phredgeno.end())  ){ //not found in consensus, could be due to deletion 
	    continue;
	}

	//cout<<"pre i"<<i<<"\t"<<infoPPos[i].posAlign<<endl;
	diNucleotideProb priorDiNuc;
	bool hasPriorAboveThreshold=false;
	//computing prior
	//                 p(nuc1) is the prob. of consensus                  *  p(contaminant)
	for(unsigned int nuc1=0;nuc1<4;nuc1++){ //     b = consensus
	    for(unsigned int nuc2=0;nuc2<4;nuc2++){//  c = contaminant
		double priortemp = (1-pos2phredgeno[ infoPPos[i].posAlign ].perror[nuc1]) * freqFromFile[ infoPPos[i].posAlign ].f[nuc2];
		priorDiNuc.p[nuc1][nuc2] = priortemp;
		// if(i==3105){
		// cout<<"i"<<i<<"\t"<<nuc1<<"\t"<<nuc2<<"\t"<<priortemp<<"\t"<<(1-pos2phredgeno[ infoPPos[i].posAlign ].perror[nuc1])<<"\t"<<freqFromFile[ infoPPos[i].posAlign ].f[nuc2]<<"\t"<<infoPPos[i].posAlign<<endl;			    			
		// }
		if(nuc1 != nuc2){
		    if(priortemp > MINPRIOR){
			//cout<<"i"<<i<<"\t"<<nuc1<<"\t"<<nuc2<<"\t"<<priortemp<<endl;
			hasPriorAboveThreshold=true;
		    }
		}

	    }
	}
	priorDiNucVec[ i ] = priorDiNuc;
	// if(i==3105){ exit(1); }
	infoPPos[i].skipPosition = (!hasPriorAboveThreshold);



	vector<diNucleotideProb> probConsVecToAdd;
	vector<diNucleotideProb> probContVecToAdd;


	// continue;
	for(unsigned int k=0;k<infoPPos[i].readsVec.size();k++){ //for every read at that position
	    diNucleotideProb   probConsVec;
	    diNucleotideProb   probContVec;
	   
	    int baseIndex = baseResolved2int(infoPPos[i].readsVec[k].base);
	    int qual      = infoPPos[i].readsVec[k].qual;
	    int dist5p    = infoPPos[i].readsVec[k].dist5p;
	    int dist3p    = infoPPos[i].readsVec[k].dist3p;
	    //int mapq      = infoPPos[i].readsVec[k].mapq;

	    probSubstition * probSubMatchToUseCons = &defaultSubMatch ;
	    probSubstition * probSubMatchToUseCont = &defaultSubMatch ; //leave it that way, we only allow deamination for the endogenous only
		    

	    //consider deamination to be possible for the endogenous only
	    if(dist5p <= (int(sub5p.size()) -1)){
		probSubMatchToUseCons = &sub5p[ dist5p ];			
	    }
		    
	    if(dist3p <= (int(sub3p.size()) -1)){
		probSubMatchToUseCons = &sub3p[ dist3p ];
	    }
		    
	    //we have substitution probabilities for both... take the closest
	    if(dist5p <= (sub5p.size() -1) &&
	       dist3p <= (sub3p.size() -1) ){
			
		if(dist5p < dist3p){
		    probSubMatchToUseCons = &sub5p[ dist5p ];
		}else{
		    probSubMatchToUseCons = &sub3p[ dist3p ];
		}
			
	    }


	    //iterate over each possible consensus and contaminant base
	    for(int nuc1=0;nuc1<4;nuc1++){ //     b = consensus
		for(int nuc2=0;nuc2<4;nuc2++){//  c = contaminant
		    //skip when contaminant is the consensus
		    if(nuc1 == nuc2)
			continue;


		    /////////////////
		    //Consensus base/
		    /////////////////

		    //                  model*4  + obs
		    int dinucIndexCons = nuc1*4+baseIndex;

		    //                        (1-e)           *  p(sub|1-e)                                + (e) *  p(sub|1-e)
		    double probCons=likeMatchProb[qual]  * (probSubMatchToUseCons->s[dinucIndexCons] )  + (1.0 - likeMatchProb[qual])*(illuminaErrorsProb.s[dinucIndexCons]);
			
		    ///////////////////
		    //Contaminant base/
		    ///////////////////

		    //                  model*4  + obs
		    int dinucIndexCont = nuc2*4+baseIndex;

		    //                        (1-e)           *  p(sub|1-e)                             + (e) *  p(sub|1-e)
		    double probCont=likeMatchProb[qual]  * (probSubMatchToUseCont->s[dinucIndexCont] )  + (1.0 - likeMatchProb[qual])*(illuminaErrorsProb.s[dinucIndexCont]);

		    probConsVec.p[nuc1][nuc2] = probCons;
		    probContVec.p[nuc1][nuc2] = probCont;

		  
		     
		    
		}
	    }//end for each di-nucleotide
		
	    
	    probConsVecToAdd.push_back(probConsVec);
	    probContVecToAdd.push_back(probContVec);
	    
	    
	} //end for each read at that position


	probConsVec[i] = probConsVecToAdd;
	probContVec[i] = probContVecToAdd;

    } //end for each position in the genome
    cerr<<"Thread #"<<threadID2Rank[(unsigned int)pthread_self()] <<" is done with  pre-computations"<<endl;

    // if(probConsVec.size() != sizeGenome){
    // 	cerr<<"Error difference in size for vectors "<<probConsVec.size()<<"\t"<<sizeGenome<<endl;
    // 	exit(1);
    // }
    // if(probContVec.size() != sizeGenome){
    // 	cerr<<"Error difference in size for vectors "<<probContVec.size()<<"\t"<<sizeGenome<<endl;
    // 	exit(1);
    // }
    // if(priorDiNucVec.size() != sizeGenome){
    // 	cerr<<"Error difference in size for vectors "<<probContVec.size()<<"\t"<<sizeGenome<<endl;
    // 	exit(1);
    // }

    for(contaminationRate=0.0;contaminationRate<0.5;contaminationRate+=0.005){
	double logLike=0.0;
    
	for(int i=0;i<sizeGenome;i++){
	    //for(int i=261;i<=262;i++){

	    
	    if( (infoPPos[i].cov == 0) || //no coverage
		(pos2phredgeno.find( infoPPos[i].posAlign ) == pos2phredgeno.end()) ||   //not found in consensus, could be due to deletion 
		infoPPos[i].skipPosition ){ //position has a tiny prior on contamination and can be safely skipped
		continue;
	    }


	    // cout<<"after i"<<i<<"\t"<<infoPPos[i].posAlign<<endl;
	    // continue;
	    for(unsigned int k=0;k<infoPPos[i].readsVec.size();k++){ //for every read at that position
		double mappedProb  =0.0; //initialized
		double misappedProb=0.25;
	   
		int mapq      = infoPPos[i].readsVec[k].mapq;

		//iterate over each possible consensus and contaminant base
		for(int nuc1=0;nuc1<4;nuc1++){ //     b = consensus
		    for(int nuc2=0;nuc2<4;nuc2++){//  c = contaminant
			//skip when contaminant is the consensus
			if(nuc1 == nuc2)
			    continue;

			// double probForDinuc =  priorDiNuc[nuc1][nuc2]*
			//     ( ( (1.0-contaminationRate) * probCons) +
			//       ( (contaminationRate)     * probCont   ) ) ;
			// cout<<"i"<<i<<"\t"<<nuc1<<"\t"<<nuc2<<endl;			    
			// cout<<"prior    "<<priorDiNucVec[i].p[nuc1][nuc2]<<endl;
			// cout<<"probCont "<<probContVec[i][k].p[nuc1][nuc2]<<endl;
			// cout<<"probCons "<<probConsVec[i][k].p[nuc1][nuc2]<<endl;
		
		  	double probForDinuc = priorDiNucVec[i].p[nuc1][nuc2]*
			    ( ( (1.0-contaminationRate) * probConsVec[i][k].p[nuc1][nuc2]) +
			      ( (contaminationRate)     * probContVec[i][k].p[nuc1][nuc2]   ) ) ;
		     
			mappedProb+=probForDinuc;


		    }
		}//end for each di-nucleotide
		

		//        m = prob. of mismapping
		//                  (1-m)     * p(data|mapped) +           m         * p(data|mismapped)	
		double pread = (probMapping[mapq]*(mappedProb) + probMismapping[mapq]*(misappedProb) );
		// cout<<k<<"\t"<<pread<<endl;
		    
		//cout<<pread<<endl;
		// cout<<probContVec[i][k].p[nuc1][nuc2]<<endl;
		// cout<<probConsVec[i][k].p[nuc1][nuc2]<<endl;
		
		//exit(1);
		
		logLike += log  (   pread   );


	    } //end for each read at that position
	    // cout<<"logLike "<<i<<"\t"<<logLike<<endl;
	} //end for each position in the genome
	// exit(1);

	//cout.precision(15);
	//cout<<freqFileNameToUse<<"\t"<<contaminationRate<<"\t"<<logLike<<endl;
	contaminationEstimate cse;
	cse.filename           = freqFileNameToUse ;
	cse.contaminationRate  = contaminationRate ;
	cse.logLike            = logLike ;

	//toAddToLog+=(""+freqFileNameToUse+"\t"+stringify(contaminationRate)+"\t"+stringify(logLike)+"\n");
	toAddToLog.push_back(cse);
	
    }//end each contaminationRate
    // }//end for each cont freq file




    //////////////////////////////////////////////////////////////
    //                END   COMPUTATION                         //
    //////////////////////////////////////////////////////////////



    // int counterl=0;
    // int totall  =0;
    // string line;
    // ifstream myFile;
    // myFile.open(fileNameToUse.c_str(), ios::in);

    // if (myFile.is_open()){
    // 	while ( getline (myFile,line)){
    // 	    int n = destringify<int>(line); 
    // 	    if(isPrime( n )){
    // 		if(n<100){
    // 		    cout<<testVec[n]<<endl;
    // 		}
    // 		counterl++;
    // 	    }
    // 	    totall++;
    // 	}
    // 	myFile.close();
    // }else{
    // 	cerr << "Unable to open file "<<fileNameToUse<<endl;
    // 	exit(1);
    // }


    //COUNTERS
    rc = pthread_mutex_lock(&mutexCounter);
    checkResults("pthread_mutex_lock()\n", rc);

    outputToPrint.push_back(toAddToLog);
    

    rc = pthread_mutex_unlock(&mutexCounter);
    checkResults("pthread_mutex_unlock()\n", rc);


    goto checkqueue;	   


    

    
    cerr<<"Thread "<<threadID2Rank[(unsigned int)pthread_self()]<<" ended "<<endl;
    return NULL;

}



//unsigned int posFound;
//vector<positionInfo> * positionsInfoFound;
// map<string, map<unsigned int,contaminationInfo> > contFreq;

class MyPileupVisitor : public PileupVisitor {
  
    public:
    MyPileupVisitor(const RefVector& references, Fasta * fastaReference,vector<positionInformation> * infoPPos,int sizeGenome,bool ignoreMQ)
            : PileupVisitor()
	    , m_references(references)
	    , m_fastaReference(fastaReference)
	    , m_infoPPos(infoPPos)
	    , sizeGenome(sizeGenome)
	    , ignoreMQ(ignoreMQ)
        { 

	}
        ~MyPileupVisitor(void) { }
  
    // PileupVisitor interface implementation
    public:
	// prints coverage results ( tab-delimited )
        void Visit(const PileupPosition& pileupData) {

	    char referenceBase = 'N';
	    
	    unsigned int posAlign = pileupData.Position+1;
	    int posVector=int(pileupData.Position)%sizeGenome;
	    // cout<<posVector<<endl;
	    // cout<<endl<<"pos = "<<posAlign<<"\t"<<posVector;

	    if( (posAlign%100) == 0){
		//cerr<<"pos  = "<<posAlign<<endl;
	    }
	    //for some reason, we have to do -1 on the .Position
	    if ( !m_fastaReference->GetBase(pileupData.RefId, posAlign-1, referenceBase ) ) {
		cerr << "bamtools convert ERROR: pileup conversion - could not read reference base from FASTA file" << endl;
		exit(1);
	    }


	    // cout<<""<<referenceBase<<"\t"<<endl;
	    


	    //insertion in the reads/deletion in the reference, skipping
	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){		
		if( !pileupData.PileupAlignments[i].IsCurrentDeletion &&
		    pileupData.PileupAlignments[i].IsNextInsertion &&
		    (pileupData.PileupAlignments[i].InsertionLength>0)){
		    continue;
		}
	    }


	    //deletion in the reads/insertion in the reference
	    unsigned int numReads=0;

	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){		
		if( pileupData.PileupAlignments[i].IsCurrentDeletion &&
		    pileupData.PileupAlignments[i].IsNextInsertion &&
		    (pileupData.PileupAlignments[i].InsertionLength == 0)){
		    continue;
		    //cout<<"del"<<endl;
		    //m_infoPPos->at(posVector).numDel++;
		}
	    }
	    

	    // typedef struct { 
	    //     vector<singleRead> readsVec;
	    //     int cov;
	    // } positionInformation;


	    for(unsigned int i=0;i<pileupData.PileupAlignments.size();i++){		
		// cerr<<pileupData.PileupAlignments[i].Alignment.Name<<endl;	    		    
		//skip deletion in the reads/insertion in the reference
		if( pileupData.PileupAlignments[i].IsCurrentDeletion &&
		    pileupData.PileupAlignments[i].IsNextInsertion ){
		    continue;
		}
		   
		char b   = pileupData.PileupAlignments[i].Alignment.QueryBases[pileupData.PileupAlignments[i].PositionInAlignment];
		char q   = pileupData.PileupAlignments[i].Alignment.Qualities[pileupData.PileupAlignments[i].PositionInAlignment]-offsetQual;
		int  m   = int(pileupData.PileupAlignments[i].Alignment.MapQuality);
		
		if(b == 'N') //uninformative anyway...
		    continue;

		singleRead sr;
		sr.base=b;
		sr.qual=int(q);
		sr.mapq=int(m);

		sr.dist5p=-1;
		sr.dist3p=-1;
		sr.isReversed= pileupData.PileupAlignments[i].Alignment.IsReverseStrand();
		
		if( sr.isReversed  ){
		    sr.dist5p = pileupData.PileupAlignments[i].Alignment.QueryBases.size() - pileupData.PileupAlignments[i].PositionInAlignment-1;
		    sr.dist3p = pileupData.PileupAlignments[i].PositionInAlignment;
		}else{
		    sr.dist5p = pileupData.PileupAlignments[i].PositionInAlignment;
		    sr.dist3p = pileupData.PileupAlignments[i].Alignment.QueryBases.size() - pileupData.PileupAlignments[i].PositionInAlignment-1;
		}

		m_infoPPos->at(posVector).cov++;		    

		//Add mapq
		m_infoPPos->at(posVector).mapqAvg += pow(10.0, double(pileupData.PileupAlignments[i].Alignment.MapQuality)/-10.0);
		m_infoPPos->at(posVector).readsVec.push_back(sr);

		// numReads++;
		if(numReads >= MAXCOV){
		    break;
		}

	    }//end each read
	    
	    // m_infoPPos->at(posVector).mapqAvg  = m_infoPPos->at(posVector).mapqAvg/double(m_infoPPos->at(posVector).cov);
	    // m_infoPPos->at(posVector).mapqAvg  = -10.0*( log( m_infoPPos->at(posVector).mapqAvg )/log(10.0) );
	    // m_infoPPos->at(posVector).refBase  = referenceBase;
	    //cout<<"pv = "<<posVector<<"\tpa ="<<posAlign<<endl;
	    m_infoPPos->at(posVector).posAlign = int(pileupData.Position+1)%sizeGenome; //posVector;//posAlign;
	    // m_infoPPos->at(posVector).skipPosition = false;

        }
        
    private:
    RefVector m_references;
    Fasta * m_fastaReference;
    vector<positionInformation> * m_infoPPos;
    int sizeGenome;
    bool ignoreMQ;
    //        ostream*  m_out;
};










int main (int argc, char *argv[]) {





    int numberOfThreads=1;
    // string outSeq  = "/dev/stdout";
    string outLog  = "/dev/stdout";
    // string nameMT  = "MT";
    string fileFreq = "alleleFreqMT/1000g/freqHumans.dat";

    // ofstream outSeqFP ;
    ofstream outLogFP;
    // int minQual=0;
    bool ignoreMQ=false;
    string line;

    for(int i=0;i<2;i++){
        likeMatch[i]        = log1p(    -pow(10.0,2.0/-10.0) )    /log(10);         
        likeMismatch[i]     = log  (     pow(10.0,2.0/-10.0)/3.0 )/log(10);

	likeMatchProb[i]           = 1.0-pow(10.0,2.0/-10.0) ;
        likeMismatchProb[i]        =     pow(10.0,2.0/-10.0)/3.0 ;
    }


    //Computing for quality scores 2 and up
    for(int i=2;i<64;i++){
        likeMatch[i]        = log1p(    -pow(10.0,i/-10.0) )     /log(10);          
        likeMismatch[i]     = log  (     pow(10.0,i/-10.0)/3.0  )/log(10);

        likeMatchProb[i]           = 1.0-pow(10.0,i/-10.0);
        likeMismatchProb[i]        =     pow(10.0,i/-10.0)/3.0;
    }


    //Adding mismapping probability
    for(int m=0;m<64;m++){

	//m = prob of mismapping
	double incorrectMappingProb   =     pow(10.0,m/-10.0); //m
	double correctMappingProb     = 1.0-pow(10.0,m/-10.0); //1-m


	probMapping[m]    = correctMappingProb;    //1-m
	probMismapping[m] = incorrectMappingProb;  //m

#ifdef DEBUG1
	cerr<<"m\t"<<m<<"\t"<<incorrectMappingProb<<"\t"<<correctMappingProb<<endl;
#endif
	
    	for(int i=0;i<2;i++){
    	    likeMatchMQ[m][i]           = log(  correctMappingProb*(1.0-pow(10.0,2.0/-10.0)    ) + incorrectMappingProb/4.0   )/log(10);         
    	    likeMismatchMQ[m][i]        = log(  correctMappingProb*(    pow(10.0,2.0/-10.0)/3.0) + incorrectMappingProb/4.0   )/log(10);
    	    likeMatchProbMQ[m][i]       = correctMappingProb*(1.0-pow(10.0,2.0/-10.0)    ) + incorrectMappingProb/4.0;
    	    likeMismatchProbMQ[m][i]    = correctMappingProb*(    pow(10.0,2.0/-10.0)/3.0) + incorrectMappingProb/4.0;
    	}


    	//Computing for quality scores 2 and up
    	for(int i=2;i<64;i++){
	    //  (1-m)(1-e) + m/4  = 1-m-e+me +m/4  = 1+3m/4-e+me
    	    likeMatchMQ[m][i]         = log(  correctMappingProb*(1.0-pow(10.0,i/-10.0)    ) + incorrectMappingProb/4.0    )/log(10);    
	    //  (1-m)(e/3) + m/4  = e/3 -me/3 + m/4
    	    likeMismatchMQ[m][i]      = log(  correctMappingProb*(    pow(10.0,i/-10.0)/3.0) + incorrectMappingProb/4.0    )/log(10);    
	    
    	    likeMatchProbMQ[m][i]           = correctMappingProb*(1.0-pow(10.0,i/-10.0)    ) + incorrectMappingProb/4.0;
    	    likeMismatchProbMQ[m][i]        = correctMappingProb*(    pow(10.0,i/-10.0)/3.0) + incorrectMappingProb/4.0;
    	}


#ifdef DEBUG1
    	for(int i=0;i<64;i++){
	    cerr<<"m\t"<<m<<"\t"<<i<<"\t"<<likeMatchMQ[m][i]<<"\t"<<likeMismatchMQ[m][i]<<"\t"<<likeMatchProbMQ[m][i]<<"\t"<<likeMismatchProbMQ[m][i]<<endl;
	}
#endif

    }



    //    return 1;
    string errFile    = getCWD(argv[0])+"illuminaProf/error.prof";
    string deam5pfreq = getCWD(argv[0])+"deaminationProfile/none.prof";
    string deam3pfreq = getCWD(argv[0])+"deaminationProfile/none.prof";

    const string usage=string("\t"+string(argv[0])+
			      " [options]  [consensus file] [reference fasta file] [bam file] [cont file1] [cont file2] ..."+"\n\n"+

			      "\n\tOutput options:\n"+	

			      "\t\t"+"-o  [output log]" +"\t\t"+"Output log (default: stdout)"+"\n"+
			      //"\t\t"+"-log  [log file]" +"\t\t"+"Output log  (default: stderr)"+"\n"+
			      // "\t\t"+"-name [name]" +"\t\t\t"  +"Name  (default "+nameMT+") "+"\n"+
			      // "\t\t"+"-qual [minimum quality]" +"\t\t"  +"Filter bases with quality less than this  (default "+stringify(minQual)+") "+"\n"+
			       // "\n\tOutput options:\n"+				      
			      "\n\tDeamination options:\n"+				      

			      "\t\t"+"-deam5p [.prof file]" +"\t\t"+"5p deamination frequency (default: "+deam5pfreq+")"+"\n"+
			      "\t\t"+"-deam3p [.prof file]" +"\t\t"+"3p deamination frequency (default: "+deam3pfreq+")"+"\n"+
			      // // "\t\t"+"-deam5" +"\t\t"+"5p deamination frequency"+"\n"+
			      // // "\t\t"+"-deam3" +"\t\t"+"3p deamination frequency"+"\n"+
			      "\n\tComputation options:\n"+	
			      "\t\t"+"-err" +"\t\t\t\t"+"Illumina error profile (default: "+errFile+")"+"\n"+
			      "\t\t"+"-nomq" +"\t\t\t\t"+"Ignore mapping quality (default: "+booleanAsString(ignoreMQ)+")"+"\n"+
			      "\n\tMisc. options:\n"+	
			      "\t\t"+"-t" +"\t\t\t\t"+"Number of cores to use (default: "+stringify(numberOfThreads)+")"+"\n"+

			      // "\t\t"+"-cont [file1,file2,...]" +"\t\t"+"Contamination allele frequency (default : "+fileFreq+")"+"\n"+ 
			      // "\n\tReference options:\n"+	
			      // "\t\t"+"-l [length]" +"\t\t\t"+"Actual length of the genome used for"+"\n"+
			      // "\t\t"+"  " +"\t\t\t\t"+"the reference as been wrapped around"+"\n"+
			      // "\t\t"+"  " +"\t\t\t\t"+"by default, the length of the genome will be used "+"\n"+
			      
			      "");
			      
    if( (argc== 1) ||
	(argc== 2 && string(argv[1]) == "-h") ||
	(argc== 2 && string(argv[1]) == "-help") ||
	(argc== 2 && string(argv[1]) == "--help") ){
	cout<<"Usage:"<<endl;
	cout<<""<<endl;
	cout<<usage<<endl;
	return 1;
    }


    int lastOpt=1;
    for(int i=1;i<(argc-3);i++){ //all but the last 3 args

	if(string(argv[i])[0] != '-'  ){
	    //cout<<"end"<<i<<endl;
	    lastOpt=i;
	    break;
	}

	if(string(argv[i]) == "-err"  ){
	    errFile=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(string(argv[i]) == "-deam5p"  ){
	    deam5pfreq=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(string(argv[i]) == "-deam3p"  ){
	    deam3pfreq=string(argv[i+1]);
	    i++;
	    continue;
	}

	if(string(argv[i]) == "-t"  ){
	    numberOfThreads =destringify<int>(argv[i+1]);
	    i++;
	    continue;
	}

	// if(strcmp(argv[i],"-cont") == 0 ){
	//     fileFreq=string(argv[i+1]);
	//     i++;
	//     continue;
	// }

	if(strcmp(argv[i],"-nomq") == 0 ){
	    ignoreMQ=true;
	    continue;
	}

	cerr<<"Wrong option "<<string(argv[i])<<endl;
	return 1;

    }


    if(numberOfThreads<=0){
	cerr<<"Number of cores must be a positive integer "<<endl;
	return 1;
    }
    

    string consensusFile    = string(argv[lastOpt+0]);//fasta file
    string fastaFile        = string(argv[lastOpt+1]);//fasta file
    string bamfiletopen     = string(argv[lastOpt+2]);//bam file

    for(int i=(lastOpt+3);i<(argc);i++){ //all but the last 3 args
	//cout<<"cont "<<string(argv[i])<<endl;
	queueFilesToprocess.push(string(argv[i]));	
    }

    // cout<<bamfiletopen<<endl;
    // cout<<fastaFile<<endl;
    // cout<<consensusFile<<endl;


    // return 1;


    //    return 1;
    // outSeqFP.open(outSeq.c_str());

    // if (!outSeqFP.is_open()){
    // 	cerr << "Unable to write to seq file "<<outSeq<<endl;
    // 	return 1;
    // }

    outLogFP.open(outLog.c_str());

    if (!outLogFP.is_open()){
	cerr << "Unable to write to qual file "<<outLog<<endl;
	return 1;
    }






    ////////////////////////////////////////////////////////////////////////
    //
    // BEGIN READING ERROR PROFILE
    //
    ////////////////////////////////////////////////////////////////////////

    readIlluminaError(errFile,illuminaErrorsProb);

    // for(int nuc1=0;nuc1<4;nuc1++){
    // 	for(int nuc2=0;nuc2<4;nuc2++){
	    
    // 	    cout<<illuminaErrorsProb.s[nuc2+nuc1*4]<<"\t";
    // 	}
    // 	cout<<endl;
    // }
    // return 1;
    
    ////////////////////////////////////////////////////////////////////////
    //
    // END  READING ERROR PROFILE
    //
    ////////////////////////////////////////////////////////////////////////











    ////////////////////////////////////////////////////////////////////////
    //
    // BEGIN DEAMINATION PROFILE
    //
    ////////////////////////////////////////////////////////////////////////
    readNucSubstitionFreq(deam5pfreq,sub5p);
    readNucSubstitionFreq(deam3pfreq,sub3p);
   

    int defaultSubMatchIndex=0;

    for(int nuc1=0;nuc1<4;nuc1++){
    	for(int nuc2=0;nuc2<4;nuc2++){	    
    	    if(nuc1==nuc2)
		defaultSubMatch.s[ defaultSubMatchIndex++ ] = 1.0;
	    else
		defaultSubMatch.s[ defaultSubMatchIndex++ ] = 0.0;
    	}
    	
    }


    
    ////////////////////////////////////////////////////////////////////////
    //
    // END  DEAMINATION PROFILE
    //
    ////////////////////////////////////////////////////////////////////////









    readMTConsensus(consensusFile,pos2phredgeno,sizeGenome);

    // cout<<vectorToString(allKeysMap(pos2phredgeno))<<endl;

    
    // for(int i=0;i<=sizeGenome;i++){
    // 	cout<<i<<"\t"<<(pos2phredgeno.find(i)==pos2phredgeno.end())<<endl;
    // }
    // return 1;
    // typedef struct { 
    //     vector<singleRead> readsVec;
    //     double mapqAvg;
    //     int cov;
    // } positionInformation;
    for(int i=0;i<=sizeGenome;i++){
	 positionInformation toadd;

	 toadd.cov            = 0;
	 toadd.mapqAvg        = 0.0;
	 
	 infoPPos.push_back(toadd);
     }
    //return 1;





     Fasta fastaReference;
     if ( !fastaReference.Open(fastaFile , fastaFile+".fai") ){ 
	 cerr << "ERROR: failed to open fasta file " <<fastaFile<<" and index " << fastaFile<<".fai"<<endl;
	 return false;
     }


    cerr<<"Begin reading BAM file ..."<<endl;

    BamReader reader;

    if ( !reader.Open(bamfiletopen) ) {
	cerr << "Could not open input BAM files " <<bamfiletopen<< endl;
    	return 1;
    }



    //  // retrieve reference data
    const RefVector  references = reader.GetReferenceData();


    MyPileupVisitor* cv = new MyPileupVisitor(references,&fastaReference,&infoPPos,sizeGenome,ignoreMQ);
    PileupEngine pileup;
    pileup.AddVisitor(cv);


    BamAlignment al;
    unsigned int numReads=0;
    while ( reader.GetNextAlignment(al) ) {
	// cerr<<al.Name<<endl;
	numReads++;
	if(numReads !=0 && (numReads%100000)==0){
	    cerr<<"Processed "<<thousandSeparator(numReads)<<" reads"<<endl;
	}

	if(al.IsMapped() && !al.IsFailedQC()){
	     // cerr<<al.Name<<endl;
	    pileup.AddAlignment(al);
	}
	
    }
    //    cerr<<"done"<<endl;

    


    //clean up
    pileup.Flush();

    reader.Close();
    fastaReference.Close();



    cerr<<"... done reading BAM file"<<endl;
    














    pthread_t             thread[numberOfThreads];
    int                   rc=0;


    doneReading=true;    

    for(int i=0;i<numberOfThreads;i++){
	rc = pthread_create(&thread[i], NULL, mainContaminationThread, NULL);
	checkResults("pthread_create()\n", rc);
    }


    //threads are running here

    //waiting for threads to finish
    for (int i=0; i <numberOfThreads; ++i) {
	rc = pthread_join(thread[i], NULL);
	checkResults("pthread_join()\n", rc);
    }

    // cout<<counter<<"\t"<<total<<endl;
    //outLogFP<<vectorToString(outputToPrint,"\n")<<endl;
    //outLogFP<<vectorToString(outputToPrint,"\n")<<endl;
    for(unsigned int i=0;i<outputToPrint.size();i++){
	for(unsigned int j=0;j<outputToPrint[i].size();j++){
	    outLogFP<<outputToPrint[i][j].filename<<"\t"<<outputToPrint[i][j].contaminationRate<<"\t"<<outputToPrint[i][j].logLike<<endl;
	}
    }




    // string genomeToPrint="";
    // outLogFP<<"pos\trefBase\tbase\tqual\tavgmapq\tcov\tsupp\n";
    // //genomeRef




    outLogFP.close();

    delete cv;



    return 0;
}
