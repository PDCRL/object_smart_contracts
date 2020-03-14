#include <iostream>
#include <thread>
#include "Util/Timer.cpp"
#include "Contract/Ballot.cpp"
#include "Graph/Lockfree/Graph.cpp"
#include "Util/FILEOPR.cpp"

#define maxThreads 128
#define maxPObj 1000
#define maxVObj 40000
#define funInContract 5
#define pl "=================================================================\n"
#define MValidation true    //! true or false
#define numValidator 50
#define NumBlock 26         //! at least two blocks, the first run is warmup run.
#define malMiner true       //! set the flag to make miner malicious.
#define NumOfDoubleSTx 2    //! # double-voting Tx for malicious final state by Miner, multiple of 2.

using namespace std;
using namespace std::chrono;
int    nProposal = 2;      //! nProposal: number of proposal shared objects;
int    nVoter    = 1;      //! nVoter: number of voter shared objects;
int    nThread   = 1;      //! nThread: total number of concurrent threads; default is 1.
int    numAUs;             //! numAUs: total number of Atomic Unites to be executed.
double lemda;              //! λ: random delay seed.

double totalTime[2];       //! total time taken by miner and validator algorithm.
Ballot *ballot;            //! smart contract.
Graph  *cGraph;            //! conflict grpah generated by miner to be given to validator.
int    *aCount;            //! aborted transaction count.
float_t *mTTime;           //! time taken by each miner Thread to execute AUs (Transactions).
float_t *vTTime;           //! time taken by each validator Thread to execute AUs (Transactions).
float_t *gTtime;           //! time taken by each miner Thread to add edges and nodes in the conflict graph.
vector<string>listAUs;     //! holds AUs to be executed on smart contract: "listAUs" index+1 represents AU_ID.
std::atomic<int>currAU;    //! used by miner-thread to get index of Atomic Unit to execute.
std::atomic<int>gNodeCount;//! # of valid AU node added in graph (invalid AUs will not be part of the graph & conflict list).
std::atomic<int>eAUCount;  //! used by validator threads to keep track of how many valid AUs executed by validator threads.
std::atomic<int>*mAUT;     //! array to map AUs to Trans id (time stamp); mAUT[index] = TransID, index+1 = AU_ID.
Graph  *nValBG;            //! used to store graph of respective n validators.
int *mProposalState;
int *vProposalState;
int *mVoterState;
int *vVoterState;
string *proposalNames;



/*************************Barrier code begins**********************************/
std::mutex mtx;
std::mutex pmtx; // to print in concurrent scene
std::condition_variable cv;
bool launch = false;
void wait_for_launch()
{
	std::unique_lock<std::mutex> lck(mtx);
	while (!launch) cv.wait(lck);
}

void shoot()
{
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
	Miner(int chairperson)
	{
		cGraph = new Graph();
		//! initialize the counter used to execute the numAUs to
		//! 0, and graph node counter to 0 (number of AUs added
		//! in graph, invalid AUs will not be part of the grpah).
		currAU     = 0;
		gNodeCount = 0;

		//! index location represents respective thread id.
		mTTime = new float_t [nThread];
		gTtime = new float_t [nThread];
		aCount = new int [nThread];
		
		proposalNames = new string[nProposal+1];
		for(int x = 0; x <= nProposal; x++)
		{
			proposalNames[x] = "X"+to_string(x+1);
		}
		
		for(int i = 0; i < nThread; i++) 
		{
			mTTime[i] = 0;
			gTtime[i] = 0;
			aCount[i] = 0;
		}
		
		//! Id of the contract creater is \chairperson = 0\.
		ballot = new Ballot( proposalNames, chairperson, nVoter, nProposal);
	}


	//!------------------------------------------------------------------------- 
	//!!!!!!!! MAIN MINER:: CREATE MINER + GRAPH CONSTRUCTION THREADS !!!!!!!!!!
	//!-------------------------------------------------------------------------
	void mainMiner()
	{
		Timer mTimer;
		thread T[nThread];

		//! Give \`voter\` the right to vote on this ballot.
		//! giveRightToVote_m() is serial.
		for(int voter = 1; voter <= nVoter; voter++) 
		{
			//! 0 is chairperson.
			ballot->giveRightToVote_m(0, voter);
		}

		//!-----------------------------------------------------------
		//!!!!!!!!!!    Create nThread Miner threads      !!!!!!!!!!
		//!-----------------------------------------------------------
		double start = mTimer.timeReq();
		for(int i = 0; i < nThread; i++)
			T[i] = thread(concMiner, i, numAUs, cGraph);

		for(auto& th : T) th.join();//! miner thread join

		//! Stop clock
		totalTime[0] = mTimer.timeReq() - start;

		//! print conflict grpah.
//		cGraph->print_grpah();

		//! print the final state of the shared objects.
		finalState();
//		ballot->winningProposal_m();
//		string winner;
//		ballot->winnerName_m(&winner);
	}

	//!--------------------------------------------------------
	//! The function to be executed by all the miner threads. !
	//!--------------------------------------------------------
	static void concMiner( int t_ID, int numAUs, Graph *cGraph)
	{
		Timer thTimer;

		//! flag is used to add valid AUs in Graph.
		//! (invalid AU: senders doesn't have
		//! sufficient balance to send).
		bool flag = true;

		//! get the current index, and increment it.
		int curInd = currAU++;

		//! statrt clock to get time taken by this.AU
		auto start = thTimer._timeStart();
		
		while(curInd < numAUs)
		{
			//! trns_id of STM_BTO_trans that 
			//! successfully executed this AU.
			int t_stamp;

			//! trans_ids with which
			//! this AU.trans_id is conflicting.
			list<int>conf_list;
			conf_list.clear();
			//! get the AU to execute,
			//! which is of string type.
			istringstream ss(listAUs[curInd]);

			string tmp;
			//! AU_ID to Execute.
			ss >> tmp;

			int AU_ID = stoi(tmp);

			//! Function Name (smart contract).
			ss >> tmp;

			if(tmp.compare("vote") == 0)
			{
				ss >> tmp;
				int vID = stoi(tmp);//! voter ID
				ss >> tmp;
				int pID = stoi(tmp);//! proposal ID
				
				int v = ballot->vote_m(vID, pID, &t_stamp, conf_list);
				while( v != 1 )
				{
					aCount[t_ID]++;
					v = ballot->vote_m(vID, pID, &t_stamp, conf_list);					
					if(v == -1)
					{
						//! invalid AU:sender does't have sufficent bal to send.
						flag = false;
						break;                                    
					}
				}
			}
			
			if(tmp.compare("delegate") == 0)
			{
				ss >> tmp;
				int sID = stoi(tmp);//! Sender ID
				ss >> tmp;
				int rID = stoi(tmp);//! Reciver ID

				//! execute again if tryCommit fails
				int v = ballot->delegate_m(sID, rID, 
											&t_stamp, conf_list);	
				while( v != 1 )
				{
					aCount[t_ID]++;
					v = ballot->delegate_m(sID, rID,
											&t_stamp, conf_list);
					if(v == -1)
					{
						//! invalid AU:sender does't
						//! have sufficent bal to send.
						flag = false;
						break;                                    
					}
				}
			}
			//! graph construction for committed AUs.
			if (flag == true)
			{
				mAUT[AU_ID-1] = t_stamp;
				
				//! increase graph node 
				//! counter (Valid AU executed)
				gNodeCount++;
				
				//! get respective trans conflict list using lib fun
				//list<int>conf_list = lib->get_conf(t_stamp);
				
				//!::::::::::::::::::::::::::::::::::
				//! Remove all the time stamps from :
				//! conflict list, added because of :
				//! initilization and creation of   :
				//! shared object in STM memory.    :
				//!::::::::::::::::::::::::::::::::::
				for(int y = 1; y <= (2*nVoter+nProposal+1); y++)
					conf_list.remove(y);
				
				//! statrt clock to get time taken by this.thread 
				//! to add edges and node to conflict grpah.
				auto gstart = thTimer._timeStart();

				//!------------------------------------------
				//! conf_list come from contract fun using  !
				//! pass by argument of get_bel() and send()!
				//!------------------------------------------
				//! when AU_ID conflict is empty.
				if(conf_list.begin() == conf_list.end())
				{
					Graph:: Graph_Node *tempRef;
					cGraph->add_node(AU_ID, t_stamp, &tempRef);
				}

				for(auto it = conf_list.begin(); it != conf_list.end(); it++)
				{
					int i = 0;
					//! get conf AU_ID in map table
					//! given conflicting tStamp.
					while(*it != mAUT[i]) i = (i+1)%numAUs;

					//! index start with 
					//! 0 => index+1 respresent AU_ID.
					//! cAUID = index+1, 
					//! cTstamp = mAUT[i] with this.AU_ID
					int cAUID   = i+1;
					int cTstamp = mAUT[i];

					if(cTstamp < t_stamp)
					{
						//! edge from cAUID to AU_ID.
						cGraph->add_edge(cAUID, AU_ID, cTstamp, t_stamp);
					}
					if(cTstamp > t_stamp) 
					{
						//! edge from AU_ID to cAUID.
						cGraph->add_edge(AU_ID, cAUID, t_stamp, cTstamp);
					}
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
		for(int id = 1; id <= nVoter; id++) 
			ballot->state_m(id, true, mVoterState);//for voter state

		for(int id = 1; id <= nProposal; id++) 
			ballot->state_m(id, false, mProposalState);//for Proposal state
	}
	~Miner() { };
};
/*************************Miner code ends**************************************/







/*************************Validator code begins********************************/
/*!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
! Class "Validator" CREATE & RUN "n" validator-THREAD   !
! CONCURRENTLY BASED ON CONFLICT GRPAH! GIVEN BY MINER. !
! concValidator() CALLED BY validator-THREAD TO PERFROM !
! OPERATIONS of RESPECTIVE AUs. THREAD 0 IS CONSIDERED  !
! AS MINTER-THREAD (SMART CONTRACT DEPLOYER)            !
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
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
	| create n concurrent validator threads  |
	| to execute valid AUs in conflict graph.|
	----------------------------------------*/
	void mainValidator()
	{
		for(int i = 0; i < nThread; i++) vTTime[i] = 0;
		eAUCount = 0;

		Timer vTimer;
		thread T[nThread];


		ballot->reset();

		//! giveRightToVote() function is serial.
		for(int voter = 1; voter <= nVoter; voter++) 
		{
			//! 0 is chairperson.
			ballot->giveRightToVote(0, voter);
		}

		//!------------------------------_-----------
		//!!!!! Create nThread Validator threads !!!!
		//!------------------------------------------
		//!Start clock
		double start = vTimer.timeReq();
		for(int i = 0; i<nThread; i++)	T[i] = thread(concValidator, i);

		shoot(); //notify all threads to begin the worker();

		//!validator thread join.
		for(auto& th : T) th.join( );

		//!Stop clock
		totalTime[1] = vTimer.timeReq() - start;

		//!print the final state of the shared objects by validator.
		finalState();
//		ballot->winningProposal();
//		string winner;
//		ballot->winnerName(&winner);
	}

	//!--------------------------------------------------------
	//! The function to be executed by all Validator threads. !
	//!--------------------------------------------------------
	static void concValidator( int t_ID )
	{
		//barrier to synchronise all threads for a coherent launch.
		wait_for_launch();
	
		Timer thTimer;

		//!statrt clock to get time taken by this thread.
		auto start = thTimer._timeStart();
		
		list<Graph::Graph_Node*>buffer;
		auto itr = buffer.begin();

		Graph:: Graph_Node *verTemp;
		
		while( true )
		{
			//!uncomment this to remove the effect of local buffer optimization.
			//buffer.clear();

			//! all Graph Nodes (Valid AUs executed)
			if(eAUCount == gNodeCount ) break;
			//!-----------------------------------------
			//!!!<< AU execution from local buffer. >>!!
			//!-----------------------------------------
			
			for(itr = buffer.begin(); itr != buffer.end(); itr++)
			{
				Graph::Graph_Node* temp = *itr;
				if(temp->in_count == 0)
				{
					//! expected in_degree is 0 then vertex can be executed,
					//! if not claimed by other thread.
					int expected = 0;
					if(atomic_compare_exchange_strong(
								&(temp->in_count), &expected, -1 ) == true)
					{
						//! num of Valid AUs executed is eAUCount+1.
						eAUCount++;
						
						//! get AU to execute, which is of string type;
						//! listAUs index statrt with 0 ==> -1.
						istringstream ss( listAUs[(temp->AU_ID) - 1]);
						string tmp;

						//! AU_ID to Execute.
						ss >> tmp;
						int AU_ID = stoi(tmp);


						//! Function Name (smart contract).
						ss >> tmp;
						if(tmp.compare("vote") == 0)
						{
							ss >> tmp;
							int vID = stoi(tmp);//! voter ID
							ss >> tmp;
							int pID = stoi(tmp);//! proposal ID
							int v = ballot->vote(vID, pID);
						}
						if(tmp.compare("delegate") == 0)
						{
							ss >> tmp;
							int sID = stoi(tmp);//! Sender ID
							ss >> tmp;
							int rID = stoi(tmp);//! Reciver ID
							int v = ballot->delegate(sID, rID);
						}

						
						//!-----------------------------------------
						//!change indegree of out edge nodes (node !
						//! having incomming edge from this node). !
						//!-----------------------------------------						
						Graph::EdgeNode *eTemp = temp->edgeHead->next;
						while( eTemp != temp->edgeTail)
						{
							Graph::Graph_Node* refVN =
										(Graph::Graph_Node*)eTemp->ref;

							refVN->in_count--;
							if(refVN->in_count == 0 )
							{
								//! insert into local buffer.
								buffer.push_back(refVN);
							}
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
					if(atomic_compare_exchange_strong( 
								&(verTemp->in_count), &expected, -1 ) == true)
					{
						//! num of Valid AUs executed is eAUCount+1.
						eAUCount++;
						
						//! get AU to execute, which is of string type;
						//! listAUs index statrt with 0 => -1.
						istringstream ss(listAUs[(verTemp->AU_ID)-1]);
						string tmp;

						//! AU_ID to Execute.
						ss >> tmp;
						int AU_ID = stoi(tmp);

						//! Function Name (smart contract).
						ss >> tmp;
						if(tmp.compare("vote") == 0)
						{
							ss >> tmp;
							int vID = stoi(tmp);//! voter ID
							ss >> tmp;
							int pID = stoi(tmp);//! proposal ID
							int v = ballot->vote(vID, pID);
						}
						if(tmp.compare("delegate") == 0)
						{
							ss >> tmp;
							int sID = stoi(tmp);//! Sender ID
							ss >> tmp;
							int rID = stoi(tmp);//! Reciver ID
							int v = ballot->delegate(sID, rID);
						}
						
						//!-----------------------------------------
						//!change indegree of out edge nodes (node !
						//! having incomming edge from this node). !
						//!-----------------------------------------
						Graph::EdgeNode *eTemp = verTemp->edgeHead->next;
						while( eTemp != verTemp->edgeTail)
						{
							Graph::Graph_Node* refVN = 
												(Graph::Graph_Node*)eTemp->ref;
							refVN->in_count--;
							if(refVN->in_count == 0 )
							{
								//! insert into local buffer.
								buffer.push_back( refVN );
							}
							eTemp = eTemp->next;
						}
					}
				}
				verTemp = verTemp->next;
			}
		}
		buffer.clear();
		vTTime[t_ID] = vTTime[t_ID] + thTimer._timeStop(start);
	}


	//!-------------------------------------------------
	//!FINAL STATE OF ALL THE SHARED OBJECT. Once all  |
	//!AUs executed. we are geting this using get_bel()|
	//!-------------------------------------------------
	void finalState()
	{
		for(int id = 1; id <= nVoter; id++) 
			ballot->state(id, true, vVoterState);//for voter state

		for(int id = 1; id <= nProposal; id++) 
			ballot->state(id, false, vProposalState);//for Proposal state
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
	ss >> trns1;//function name
	ss >> trns1; //! Voter ID.
	int s_id = stoi(trns1);
	ss >> trns1; //! Proposal ID.
	int r_id = stoi(trns1);

	istringstream ss1(listAUs[atPoss-1]);
	ss1 >> trns1; //! AU_ID to Execute.
	int AU_ID2 = stoi(trns1);
	ss1 >> trns1;//function name
	ss1 >> trns1; //! Voter ID.
	int s_id1 = stoi(trns1);
	ss1 >> trns1; //! Proposal ID.
	int r_id1 = stoi(trns1);

	
	Graph:: Graph_Node *tempRef;
	if(mAUT[AU_ID1-1] != 0) {
		int ts = mAUT[AU_ID1-1]+1;
		cGraph->add_node(AU_ID2, ts, &tempRef);
		mAUT[AU_ID2-1] = ts;
		gNodeCount++;
	}
	else {
		int ts = mAUT[AU_ID2-1]+1;
		cGraph->add_node(AU_ID1, ts, &tempRef);
		mAUT[AU_ID1-1] = ts;
		gNodeCount++;
	}
	
	mProposalState[r_id-1]  = 1;
	mProposalState[r_id1-1] = 1;
	mVoterState[s_id1-1]    = 1;
	return true;
}



bool stateVal() {
	//State Validation
	bool flag = false;
//	cout<<"\n"<<pl<<"Proposal \tMiner \t\tValidator"<<endl;
	for(int pid = 0; pid < nProposal; pid++) {
//		cout<<pid+1<<" \t \t"<<mProposalState[pid]
//			<<" \t\t"<<vProposalState[pid]<<endl;
		if(mProposalState[pid] != vProposalState[pid])
			flag = true;
	}
//	cout<<"\n"<<pl<<"Voter ID \tMiner \t\tValidator"<<endl;
	for(int vid = 0; vid < nVoter; vid++) {
//		cout<<vid+1<<" \t \t"<<mVoterState[vid]
//			<<" \t\t"<<vVoterState[vid]<<endl;
		if(mVoterState[vid] != vVoterState[vid])
			flag = true;
	}
	return flag;
}





/*************************Main Fun code begins*********************************/
/*!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
/*!!!!!!!!          main()         !!!!!!!!!!*/
/*!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
int main( )
{
	cout<<pl<<"MVOSTM Miner and Decentralized Validator\n";
	cout<<"--------------------------------\n";
	//! list holds the avg time taken by miner and Validator
	//! thread s for multiple consecutive runs.
	list<double>mItrT;
	list<double>vItrT;
	int totalDepInG  = 0; //to get total number of dependencies in graph;
	int totalRejCont = 0; //number of validator rejected the blocks;
	int maxAccepted  = 0;
	int totalRun     = NumBlock; //at least 2

	FILEOPR file_opr;

	//! read from input file:: nProposal = #numProposal; nThread = #threads;
	//! numAUs = #AUs; λ = random delay seed.
	file_opr.getInp(&nProposal, &nVoter, &nThread, &numAUs, &lemda);

	if(nProposal > maxPObj) {
		nProposal = maxPObj;
		cout<<"Max number of Proposals can be "<<maxPObj<<"\n";
	}
	if(nVoter > maxVObj) {
		nVoter = maxVObj;
		cout<<"Max number of Voters can be "<<maxVObj<<"\n";
	}

	mProposalState   = new int [nProposal];
	vProposalState   = new int [nProposal];
	mVoterState      = new int [nVoter];
	vVoterState      = new int [nVoter];

	for(int numItr = 0; numItr < totalRun; numItr++)
	{
		 //! generates AUs (i.e. trans to be executed by miner & validator).
		file_opr.genAUs(numAUs, nVoter, nProposal, funInContract, listAUs);
		//! index+1 represents respective AU id, and
		//! mAUT[index] represents "time stamp (commited trans)".
		mAUT = new std::atomic<int>[numAUs];
		for(int i = 0; i< numAUs; i++) mAUT[i] = 0;
		Timer mTimer;
		mTimer.start();

		//MINER
		Miner *miner = new Miner(0);//0 is contract deployer id
		miner ->mainMiner();

		//Function to add malicious trans and final state by Miner
		if(lemda != 0) bool rv = addMFS(NumOfDoubleSTx);

		int totalEdginBG = cGraph->print_grpah();//dependenices in the graph.
		if(numItr > 0) totalDepInG += totalEdginBG;

		//VALIDATOR
		if(MValidation == true)
		{
			int acceptCount = 0, rejectCount = 0;
			for(int nval = 0; nval < numValidator; nval++)
			{
				for(int p = 0; p < nProposal; p++) vProposalState[p] = 0;
				for(int v = 0; v < nVoter; v++) vVoterState[v] = 0;

				Validator *validator = new Validator();
				nValBG = NULL;
				nValBG = new Graph;
				cGraph->copy_graph(nValBG);

				//If the miner is malicious this
				//fun remove an edge from graph.
				if(malMiner == true)
				{
					int eTR = ceil((totalEdginBG * lemda)/100);
					for(int r = 0; r < eTR; r++)
						nValBG->remove_Edge(nValBG);
				}

				validator ->mainValidator();

				//State Validation
				bool flag = stateVal();
				if(flag == true) rejectCount++;
				else acceptCount++;
			}
			if(numItr > 0 && malMiner == true) {
				totalRejCont += rejectCount;
				if(maxAccepted < acceptCount ) maxAccepted = acceptCount;
			}
		}
		else
		{
			Validator *validator = new Validator();
			nValBG = new Graph;
			cGraph->copy_graph(nValBG);
			validator ->mainValidator();
			//State Validation
			bool flag = stateVal();
			if(flag == true) cout<<"\nBlock Rejected by Validator";
		}
		int abortCnt = 0;
		for( int iii = 0; iii < nThread; iii++ ) {
			abortCnt = abortCnt + aCount[iii];
		}
//		if(numItr > 0)cout<<"\nNumber of STM Transaction Aborted "<<abortCnt;

		mTimer.stop();

		float_t gConstT = 0;
		for(int ii = 0; ii < nThread; ii++) gConstT += gTtime[ii];
//		cout<<"Avg Grpah Time= "<<gConstT/nThread<<" microseconds";

		//! total valid AUs among total AUs executed 
		//! by miner and varified by Validator.
		int vAUs = gNodeCount;
		if(numItr > 0)
		file_opr.writeOpt(nProposal, nVoter, nThread, numAUs, totalTime,
		                  mTTime, vTTime, aCount, vAUs, mItrT, vItrT);

		for(int p = 0; p < nProposal; p++) mProposalState[p] = 0;
		for(int v = 0; v < nVoter; v++) mVoterState[v] = 0;
		listAUs.clear();
		delete miner;
		miner  = NULL;
		delete cGraph;
		cGraph = NULL;
	}
	//! to get total avg miner and validator
	//! time after number of totalRun runs.
	double tAvgMinerT = 0, tAvgValidT = 0;
	auto mit = mItrT.begin();
	auto vit = vItrT.begin();
	for(int j = 1; j < totalRun; j++){
		tAvgMinerT = tAvgMinerT + *mit;
		tAvgValidT = tAvgValidT + *vit;mit++;
		vit++;
	}
	tAvgMinerT = tAvgMinerT/(totalRun-1);
	tAvgValidT = tAvgValidT/(totalRun-1);
	
	cout<<"    Total Avg Miner       = "<<tAvgMinerT<<" microseconds";
	cout<<"\nTotal Avg Validator       = "<<tAvgValidT<<" microseconds";
	cout<<"\n--------------------------------";
	cout<<"\nAvg Dependencies in Graph = "<<totalDepInG/(totalRun-1);
	cout<<"\n--------------------------------";
	cout<<"\nAvg Number of Validator Accepted the Block = "
		<<(numValidator-(totalRejCont/(totalRun-1)));
	cout<<"\nAvg Number of Validator Rejcted the Block  = "
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