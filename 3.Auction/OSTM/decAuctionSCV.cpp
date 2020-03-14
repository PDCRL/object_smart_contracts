#include <iostream>
#include <thread>
#include "Util/Timer.cpp"
#include "Contract/SimpleAuctionSCV.cpp"
#include "Graph/Lockfree/Graph.cpp"
#include "Util/FILEOPR.cpp"

#define maxThreads 128
#define maxBObj 5000
#define maxbEndT 5000 //millseconds
#define funInContract 6
#define pl "=================================================================\n"
#define MValidation true  //! true or false
#define numValidator 50
#define NumBlock 26     //! at least two blocks, the first run is warmup run.
#define malMiner true    //! set the flag to make miner malicious.
#define NumOfDoubleSTx 2  //! # double-spending Tx for malicious final state by Miner, multiple of 2.

using namespace std;
using namespace std::chrono;

int beneficiary = 0;           //! fixed beneficiary id to 0, it can be any unique address/id.
int    nBidder  = 2;           //! nBidder: number of bidder shared objects.
int    nThread  = 1;           //! nThread: total number of concurrent threads; default is 1.
int    numAUs;                 //! numAUs: total number of Atomic Unites to be executed.
double lemda;                  //! λ: random delay seed.
int    bidEndT;                //! time duration for auction.
double tTime[2];              //! total time taken by miner and validator algorithm respectively.
SimpleAuction *auction = NULL; //! smart contract for miner.
Graph  *cGraph = NULL;         //! conflict grpah generated by miner to be given to validator.
int    *aCount = NULL;         //! aborted transaction count.
float_t *mTTime = NULL;        //! time taken by each miner Thread to execute AUs (Transactions).
float_t *vTTime = NULL;        //! time taken by each validator Thread to execute AUs (Transactions).
float_t *gTtime = NULL;        //! time taken by each miner Thread to add edges and nodes in the conflict graph.
vector<string> listAUs;        //! holds AUs to be executed on smart contract: "listAUs" index+1 represents AU_ID.
std::atomic<int> currAU;       //! used by miner-thread to get index of Atomic Unit to execute.
std::atomic<int> gNodeCount;   //! # of valid AU node added in graph (invalid AUs will not be part of the graph & conflict list).
std::atomic<int> eAUCount;     //! used by validator threads to keep track of how many valid AUs executed by validator threads.
std::atomic<int> *mAUT = NULL; //! array to map AUs to Trans id (time stamp); mAUT[index] = TransID, index+1 = AU_ID.
Graph  *nValBG;                //! used to store graph of respective n validators.
std::atomic<bool>mm;           //! miner is malicious, this is used by validators.


// state
int mHBidder;
int mHBid;
int vHBidder;
int vHBid;
int *mPendingRet;
int *vPendingRet;



/*************************Barrier code begins**********************************/
std::mutex mtx;
std::mutex pmtx; // to print in concurrent scene
std::condition_variable cv;
bool launch = false;

void wait_for_launch() {
	std::unique_lock<std::mutex> lck(mtx);
	while (!launch) cv.wait(lck);
}

void shoot() {
	std::unique_lock<std::mutex> lck(mtx);
	launch = true;
	cv.notify_all();
}
/*************************Barrier code ends************************************/



/*************************Miner code begins************************************/
/*!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!    Class "Miner" CREATE & RUN "n" miner-THREAD CONCURRENTLY           !
!"concMiner()" CALLED BY miner-THREAD TO PERFROM oprs of RESPECTIVE AUs !
! THREAD 0 IS CONSIDERED AS MINTER-THREAD (SMART CONTRACT DEPLOYER)     !
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
class Miner
{
	public:
	Miner( )
	{
		cGraph = NULL;
		cGraph = new Graph();
		//! initialize the counter used to execute the numAUs to
		//! 0, and graph node counter to 0 (number of AUs added in
		//! graph, invalid AUs will not be a part of the grpah).
		currAU     = 0;
		gNodeCount = 0;

		//! index location represents respective thread id.
		mTTime = new float_t [nThread];
		gTtime = new float_t [nThread];
		aCount = new int [nThread];

		for(int i = 0; i < nThread; i++) {
			mTTime[i] = 0;
			gTtime[i] = 0;
			aCount[i] = 0;
		}
		auction = new SimpleAuction(bidEndT, beneficiary, nBidder);
	}

	//!-------------------------------------------- 
	//!!!!!! MAIN MINER:: CREATE MINER THREADS !!!!
	//!--------------------------------------------
	void mainMiner()
	{
		Timer mTimer;
		thread T[nThread];

		//!------------------------------------------
		//!!!!!! CREATE nThreads MINER THREADS  !!!!!
		//!------------------------------------------
		double start = mTimer.timeReq();
		for(int i = 0; i < nThread; i++)
			T[i] = thread(concMiner, i, numAUs, cGraph);
		for(auto& th : T) th.join();
		tTime[0] = mTimer.timeReq() - start;

		//! print conflict grpah.
//		cGraph->print_grpah();

		//! print the final state of the shared objects.
		finalState();
//		 auction->AuctionEnded_m( );
	}

	//!--------------------------------------------------------
	//! THE FUNCTION TO BE EXECUTED BY ALL THE MINER THREADS. !
	//!--------------------------------------------------------
	static void concMiner( int t_ID, int numAUs, Graph *cGraph)
	{
		Timer thTimer;
		//! flag is used to add valid AUs in Graph.
		//! (invalid AU: senders doesn't have sufficient balance to send).
		bool flag = true;
		//! get the current index, and increment it.
		int curInd = currAU++;
		//! statrt clock to get time taken by this.AU
		auto start = thTimer._timeStart();
		while(curInd < numAUs)
		{
			//! trns_id of STM_BTO_trans that successfully executed this AU.
			int t_stamp;
			//! trans_ids with which this AU.trans_id is conflicting.
			list<int>conf_list;
			conf_list.clear();
			istringstream ss(listAUs[curInd]);
			string tmp;
			ss >> tmp;
			int AU_ID = stoi(tmp);
			ss >> tmp;
			if(tmp.compare("bid") == 0)
			{
				ss >> tmp;
				int payable = stoi(tmp);//! payable
				ss >> tmp;
				int bID = stoi(tmp);//! Bidder ID
				ss >> tmp;
				int bAmt = stoi(tmp);//! Bidder value
				int v = auction->bid_m(payable, bID, bAmt, &t_stamp, conf_list);
				while(v != 1) {
					aCount[0]++;
					v = auction->bid_m(payable, bID, bAmt, &t_stamp, conf_list);
					if(v == -1) {
						flag = false;//! invalid AU.
						break;                                    
					}
				}
			}
			if(tmp.compare("withdraw") == 0)
			{
				ss >> tmp;
				int bID = stoi(tmp);//! Bidder ID

				int v = auction->withdraw_m(bID, &t_stamp, conf_list);
				while(v != 1) {
					aCount[0]++;
					v = auction->withdraw_m(bID, &t_stamp, conf_list);
					if(v == -1) {
						flag = false;//! invalid AU.
						break;                                    
					}
				}
			}
			if(tmp.compare("auction_end") == 0)
			{
				int v = auction->auction_end_m(&t_stamp, conf_list);
				while(v != 1) {
					aCount[0]++;
					v = auction->auction_end_m(&t_stamp, conf_list);
					if(v == -1) {
						flag = false;//! invalid AU.
						break;                                    
					}
				}
			}
			//! graph construction for committed AUs.
			if (flag == true)
			{
				mAUT[AU_ID-1] = t_stamp;
				gNodeCount++;//! increase graph node counter (Valid AU executed)
				//! get respective trans conflict list using lib fun
				//list<int>conf_list = lib->get_conf(t_stamp);
				
				//!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
				//! Remove all the time stamps from conflict list, added because 
				//! of initilization and creation of shared object in STM memory
				//!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
				for(int y = 0; y <= 1; y++) conf_list.remove(y);

				//! statrt clock to get time taken by this.thread 
				//! to add edges and node to conflict grpah.
				auto gstart = thTimer._timeStart();

				//!------------------------------------------
				//! conf_list come from contract fun using  !
				//! pass by argument of get_bel() and send()!
				//!------------------------------------------
				//! when AU_ID conflict is empty.
				if(conf_list.begin() == conf_list.end()) {
					Graph:: Graph_Node *tempRef;
					cGraph->add_node(AU_ID, t_stamp, &tempRef);
				}

				for(auto it = conf_list.begin(); it != conf_list.end(); it++)
				{
					int i = 0;
					//! get conf AU_ID in map table given conflicting tStamp.
					while(*it != mAUT[i]) i = (i+1)%numAUs;
					//! index start with 0 => index+1 respresent AU_ID.
					//! cAUID = index+1, cTstamp = mAUT[i] with this.AU_ID
					int cAUID   = i+1;
					int cTstamp = mAUT[i];
					if(cTstamp < t_stamp) //! edge from cAUID to AU_ID.
						cGraph->add_edge(cAUID, AU_ID, cTstamp, t_stamp);
					if(cTstamp > t_stamp) //! edge from AU_ID to cAUID.
						cGraph->add_edge(AU_ID, cAUID, t_stamp, cTstamp);
				}
				gTtime[t_ID] = gTtime[t_ID] + thTimer._timeStop(gstart);
			}
			//! reset flag for next AU.
			flag = true;
			//! get the current index to execute, and increment it.
			curInd = currAU++;
			conf_list.clear();
		}
		mTTime[t_ID] = mTTime[t_ID] + thTimer._timeStop(start);
	}

	//!-------------------------------------------------
	//!FINAL STATE OF ALL THE SHARED OBJECT. Once all  |
	//!AUs executed. we are geting this using state_m()|
	//!-------------------------------------------------
	void finalState()
	{	
		auction->state_m(&mHBidder, &mHBid, mPendingRet);
	}
	~Miner() { };
};
/*************************Miner code ends**************************************/




/*************************Validator code begins********************************/
//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
//! Class "Validator" CREATE & RUN "n" validator-THREAD CONCURRENTLY !
//! BASED ON CONFLICT GRPAH GIVEN BY MINER. "concValidator()" CALLED !
//! BY validator-THREAD TO PERFROM OPERATIONS of RESPECTIVE AUs.     ! 
//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
class Validator
{
	public:
	Validator()
	{
		//! int the execution counter used by validator threads.
		eAUCount = 0;
		//! array index location represents respective thread id.
		vTTime = new float_t [nThread];
		for(int i = 0; i < nThread; i++) vTTime[i] = 0;
	};


	/*!---------------------------------------
	| CREATE N CONCURRENT VALIDATOR THREADS  |
	| TO EXECUTE VALID AUS IN CONFLICT GRAPH.|
	----------------------------------------*/
	void mainValidator()
	{
		Timer vTimer;
		thread T[nThread];
		auction->reset(bidEndT);
		//!------------------------------------------
		//!!!!! CREATE nThread VALIDATOR THREADS !!!!
		//!------------------------------------------
		double start = vTimer.timeReq();
		for(int i = 0; i<nThread; i++)
			T[i] = thread(concValidator, i);
		for(auto& th : T) th.join( );
		tTime[1] = vTimer.timeReq() - start;

	//!print the final state of the shared objects by validator.
		finalState();
//		auction->AuctionEnded( );
	}

	//!--------------------------------------------------------
	//! THE FUNCTION TO BE EXECUTED BY ALL VALIDATOR THREADS. !
	//!--------------------------------------------------------
	static void concValidator( int t_ID )
	{
		Timer thTimer;
		auto start = thTimer._timeStart();
		list<Graph::Graph_Node*>buffer;
		auto itr = buffer.begin();
		Graph:: Graph_Node *verTemp;
		while( mm == false )
		{
			//! uncomment this to remove the effect of local buffer optimization.
			//buffer.clear();

			//! all Graph Nodes (Valid AUs executed)
			if(eAUCount == gNodeCount ) break;
			//!-----------------------------------------
			//!!!<< AU EXECUTION FROM LOCAL buffer. >>!!
			//!-----------------------------------------
			
			for(itr = buffer.begin(); itr != buffer.end(); itr++)
			{
				Graph::Graph_Node* temp = *itr;
				if(temp->in_count == 0)
				{
					//! expected in_degree is 0 then vertex can be executed,
					//! if not claimed by other thread.
					int expected = 0;
					if( atomic_compare_exchange_strong
					  ( &(temp->in_count), &expected, -1 ) == true)
					{
						//! num of Valid AUs executed is eAUCount+1.
						eAUCount++;
						//! get AU to execute, which is of string type;
						//! listAUs index statrt with 0 ==> -1.
						istringstream ss( listAUs[(temp->AU_ID) - 1]);
						string tmp;
						ss >> tmp;
						int AU_ID = stoi(tmp);
						ss >> tmp;
						if(tmp.compare("bid") == 0)
						{
							ss >> tmp;
							int payable = stoi(tmp);//! payable
							ss >> tmp;
							int bID = stoi(tmp);//! Bidder ID
							ss >> tmp;
							int bAmt = stoi(tmp);//! Bidder value
							int v = auction->bid(payable, bID, bAmt);
							if(v == -1) mm = true;
						}
						if(tmp.compare("withdraw") == 0)
						{
							ss >> tmp;
							int bID = stoi(tmp);//! Bidder ID
							int v = auction->withdraw(bID);
							if(v == -1) mm = true;
						}
						if(tmp.compare("auction_end") == 0)
						{
							bool v = auction->auction_end( );
						}

						//!------------------------------------------
						//! CHANGE INDEGREE OF OUT EDGE NODES (NODE !
						//! HAVING INCOMMING EDGE FROM THIS NODE).  !
						//!------------------------------------------
						Graph::EdgeNode *eTemp = temp->edgeHead->next;
						while( eTemp != temp->edgeTail)
						{
							Graph::Graph_Node* refVN = 
							                  (Graph::Graph_Node*)eTemp->ref;

							refVN->in_count--;
							if(refVN->in_count == 0 ) //! insert into local buf.
								buffer.push_back(refVN);
							eTemp = eTemp->next;
						}
						delete eTemp;
					}
				}
			}
			//! reached to end of local buffer; clear the buffer.
			buffer.clear();
			
			//!-----------------------------------------------------
			//!!!<< AU execution by traversing conflict grpah  >>!!!
			//!-----------------------------------------------------
			verTemp = nValBG->verHead->next;
			while(verTemp != nValBG->verTail)
			{
				if(verTemp->in_count == 0)
				{
					//! expected in_degree is 0 then vertex can be executed,
					//! if not claimed by other thread.
					int expected = 0;
					if( atomic_compare_exchange_strong
					  ( &(verTemp->in_count), &expected, -1 ) == true)
					{
						//! num of Valid AUs executed is eAUCount+1.
						eAUCount++;
						//! get AU to execute, which is of string type;
						//! listAUs index statrt with 0 => -1.
						istringstream ss(listAUs[(verTemp->AU_ID)-1]);
						string tmp;
						ss >> tmp;
						int AU_ID = stoi(tmp);
						ss >> tmp;
						if(tmp.compare("bid") == 0)
						{
							ss >> tmp;
							int payable = stoi(tmp);//! payable
							ss >> tmp;
							int bID = stoi(tmp);//! Bidder ID
							ss >> tmp;
							int bAmt = stoi(tmp);//! Bidder value
							int v = auction->bid(payable, bID, bAmt);
							if(v == -1) mm = true;
						}
						if(tmp.compare("withdraw") == 0)
						{
							ss >> tmp;
							int bID = stoi(tmp);//! Bidder ID
							int v = auction->withdraw(bID);
							if(v == -1) mm = true;
						}
						if(tmp.compare("auction_end") == 0)
						{
							bool v = auction->auction_end( );
						}
						
						//!------------------------------------------
						//! CHANGE INDEGREE OF OUT EDGE NODES (NODE !
						//! HAVING INCOMMING EDGE FROM THIS NODE).  !
						//!------------------------------------------
						Graph::EdgeNode *eTemp = verTemp->edgeHead->next;
						while( eTemp != verTemp->edgeTail) {
							Graph::Graph_Node* refVN 
							              = (Graph::Graph_Node*)eTemp->ref;
							refVN->in_count--;
							if(refVN->in_count == 0 )//!insert into local buffer
								buffer.push_back( refVN );
							eTemp = eTemp->next;
						}
					}
				}
				verTemp = verTemp->next;
			}
			//sleep(.00009);
		}
		verTemp = NULL;
		delete verTemp;
		buffer.clear();
		vTTime[t_ID] = vTTime[t_ID] + thTimer._timeStop(start);
	}


	//!-------------------------------------------------
	//!FINAL STATE OF ALL THE SHARED OBJECT. ONCE ALL  |
	//!AUS EXECUTED. WE ARE GETING THIS USING state()  |
	//!-------------------------------------------------
	void finalState()
	{
		auction->state(&vHBidder, &vHBid, vPendingRet);
	}

	~Validator() { };
};
/*************************Validator code ends**********************************/


//!--------------------------------------------------------------------------
//! atPoss:: from which double-spending Tx to be stored at end of the list. !
//! add malicious final state with double-spending Tx                       !
//!--------------------------------------------------------------------------
bool addMFS(int atPoss)
{
	istringstream ss(listAUs[atPoss-2]);
	string trns1;
	ss >> trns1; //! AU_ID to Execute.
	int AU_ID1 = stoi(trns1);
	ss >> trns1; //function name
	ss >> trns1; //! payable.
	int payable = stoi(trns1);
	ss >> trns1; //! bidder ID.
	int bidderID = stoi(trns1);
	ss >> trns1; //! Ammount to bid.
	int bidValue  = stoi(trns1);

	istringstream ss1(listAUs[atPoss-1]);
	string trns2;
	ss1 >> trns1; //! AU_ID to Execute.
	int AU_ID2 = stoi(trns1);
	ss1 >> trns1; //function name
	ss1 >> trns1; //! payable.
	int payable1 = stoi(trns1);
	ss1 >> trns1; //! bidderID.
	int bidderID1 = stoi(trns1);
	ss1 >> trns1; //! Ammount to bid.
	int bidValue1  = stoi(trns1);


//	cGraph->remove_AU_Edge(cGraph, AU_ID1);
//	cGraph->remove_AU_Edge(cGraph, AU_ID2);

	Graph:: Graph_Node *tempRef;
	int ts = 5000;
	cGraph->add_node(AU_ID1, ts, &tempRef);
	mAUT[AU_ID1-1] = ts;
	gNodeCount++;

	ts = 5001;
	cGraph->add_node(AU_ID2, ts, &tempRef);
	mAUT[AU_ID2-1] = ts;
	gNodeCount++;

	bidValue = 999;
	trns1 = to_string(AU_ID1)+" bid "+to_string(payable)+" "
			+to_string(bidderID)+" "+to_string(bidValue);
	listAUs[AU_ID1-1] =  trns1;
	bidValue1 = 1000;
	trns2 = to_string(AU_ID2)+" bid "+to_string(payable1)+" "
			+to_string(bidderID1)+" "+to_string(bidValue1);
	listAUs[AU_ID2-1] =  trns1;

	mHBidder = bidderID1;
	mHBid    = bidValue;
	mPendingRet[bidderID1] = mHBid;

	return true;
}



bool stateVal() {
	//State Validation
	bool flag = false;
	if(mHBidder != vHBidder || mHBid != vHBid) flag = true;
//	cout<<"\n============================================"
//	    <<"\n     Miner Auction Winer "<<mHBidder
//	    <<" |  Amount "<<mHBid;
//	cout<<"\n Validator Auction Winer "<<to_string(vHBidder)
//	    <<" |  Amount "<<to_string(vHBid);
//	cout<<"\n============================================\n";
	return flag;
}

/*************************Main Fun code begins*********************************/
/*!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
/*!!!!!!!!          main()         !!!!!!!!!!*/
/*!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
int main( )
{
	cout<<pl<<"OSTM Miner and Decentralised SCV\n";
	cout<<"--------------------------------\n";
	//! list holds the avg time taken by miner and Validator
	//! threads for multiple consecutive runs.
	list<double>mItrT;
	list<double>vItrT;
	int totalDepInG  = 0; //to get total number of dependencies in graph;
	int totalRejCont = 0; //number of validator rejected the blocks;
	int maxAccepted  = 0;
	int totalRun     = NumBlock; //at least 2

	FILEOPR file_opr;

	//! read from input file:: nBidder = #numProposal; nThread = #threads;
	//! numAUs = #AUs; λ = random delay seed.
	file_opr.getInp(&nBidder, &bidEndT, &nThread, &numAUs, &lemda);

	//! max Proposal shared object error handling.
	if(nBidder > maxBObj) {
		nBidder = maxBObj;
		cout<<"Max number of Proposal Shared Object can be "<<maxBObj<<"\n";
	}

	for(int numItr = 0; numItr < totalRun; numItr++)
	{
		//! generates AUs (i.e. trans to be executed by miner & validator).
		file_opr.genAUs(numAUs, nBidder, funInContract, listAUs);
		//! index+1 represents respective AU id, and
		//! mAUT[index] represents "time stamp (commited trans)".
		mAUT = new std::atomic<int>[numAUs];
		for(int i = 0; i< numAUs; i++) mAUT[i] = 0;
		tTime[0]    = 0;
		tTime[1]    = 0;
		mPendingRet = new int [nBidder+1];
		vPendingRet = new int [nBidder+1];
		Timer mTimer;
		mTimer.start();
		mm = new std::atomic<bool>;
		mm = false;

		//MINER
		Miner *miner = new Miner();
		miner ->mainMiner();

		if(lemda != 0) bool rv = addMFS(NumOfDoubleSTx);

		int totalEdginBG = cGraph->print_grpah();
		//give dependenices in the graph.
		if(numItr > 0) totalDepInG += totalEdginBG;

		//VALIDATOR
		if(MValidation == true) {
			int acceptCount = 0, rejectCount = 0;
			for(int nval = 0; nval < numValidator; nval++) {
				Validator *validator = new Validator();
				nValBG = NULL;
				nValBG = new Graph;
				cGraph->copy_graph(nValBG);

				//If the miner is malicious this
				//fun remove lemda edge from graph.
				if(malMiner == true) {
					int eTR = ceil((totalEdginBG * lemda)/100);
					for(int r = 0; r < eTR; r++)
						nValBG->remove_Edge(nValBG);
				}

				validator ->mainValidator();

				bool flag = stateVal();
				if(flag == true) rejectCount++;
				else acceptCount++;
				mm = false;
			}
			if(numItr > 0 && malMiner == true) {
				totalRejCont += rejectCount;
				if(maxAccepted < acceptCount ) maxAccepted = acceptCount;
			}
		}
		else {
			Validator *validator = new Validator();
			nValBG = new Graph;
			cGraph->copy_graph(nValBG);
			validator ->mainValidator();
			//State Validation
			bool flag = stateVal();
			if(flag == true) cout<<"\nBlock Rejected by Validator";
		}
		mTimer.stop();

		float_t gConstT = 0;
		for(int ii = 0; ii < nThread; ii++) gConstT += gTtime[ii];
//		cout<<"\nAvg Grpah Time= "<<gConstT/nThread<<" microseconds";
		
		//! total valid AUs among total AUs executed 
		//! by miner and varified by Validator.
		int vAUs = numAUs-aCount[0];
		if(numItr > 0)
			file_opr.writeOpt(nBidder, nThread, numAUs, tTime, 
			                  mTTime, vTTime, aCount, vAUs, mItrT, vItrT);

		listAUs.clear();
		delete miner;
		miner = NULL;
		delete cGraph;
		auction = NULL;
	}
	
	//to get total avg miner and validator
	//time after number of totalRun runs.
	double tAvgMinerT = 0, tAvgValidT = 0;
	auto mit = mItrT.begin();
	auto vit = vItrT.begin();
	for(int j = 1; j < totalRun; j++) {
		tAvgMinerT = tAvgMinerT + *mit;
		tAvgValidT = tAvgValidT + *vit;
		mit++;
		vit++;
	}
	tAvgMinerT = tAvgMinerT/(totalRun-1);
	tAvgValidT = tAvgValidT/(totalRun-1);

	cout<<"    Total Avg Miner       = "<<tAvgMinerT<<" microseconds";
	cout<<"\nTotal Avg Validator       = "<<tAvgValidT<<" microseconds";
	cout<<"\n--------------------------------";
	cout<<"\nAvg Dependencies in Graph = "<<totalDepInG/(totalRun-1);
	cout<<"\n--------------------------------\n";
	cout<<"Avg Number of Validator Accepted the Block = "
		<<(numValidator-(totalRejCont/(totalRun-1)));
	cout<<"\nAvg Number of Validator Rejcted the Block = "
		<<totalRejCont/(totalRun-1);
	cout<<"\nMax Validator Accepted any Block = "<<maxAccepted;
	cout<<"\n"<<endl;

	mItrT.clear();
	vItrT.clear();
	delete mTTime;
	delete vTTime;
	delete aCount;
	return 0;
}
/*************************Main Fun code ends***********************************/
