
#include <iostream>
#include <vector>
#include <utility>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include "hw2_output.h"

using namespace std;

typedef struct properPrivate{
    int gid; // unique id 
    int si; // area i-dimension
    int sj; // area j-dimension
    int tg; // time to wait
    int ng; // number of areas to clean
    vector<pair<int,int>> areas; // top-left corners of assigned areas to clean, len(areas) = ng
} Private;

typedef struct commanderInput{
    vector<pair<int,string>>* orders;
    pthread_t* tids ;
} commanderInput;

typedef struct triplet{
    int ik;
    int jk;
    int ck;
} Triplet;

typedef struct smoker{
    int sid; // unique id
    int ts; // time to wait
    int ns; // number of cigarettes
    vector<Triplet> smokeAreas; // the cells smoker will smoke
} Smoker;



////////// GLOBAL VARIABLES

int **grid; // grid
int **numOfGatherers; // if there is a gatherer
int n,m; //  grid dimensions
pthread_mutex_t gridMutex; // mutexes for each cell in grid
sem_t** gridSem; // semaphore for each cell
pthread_cond_t*** cvAvailable;
pthread_mutex_t availableLock;

pthread_mutex_t breakLock;
pthread_cond_t cvBreak = PTHREAD_COND_INITIALIZER;
int Break;
int Stop;

pthread_mutex_t contLock;
pthread_cond_t cvCont = PTHREAD_COND_INITIALIZER;

//pthread_mutex_t dummyLock;

pthread_cond_t*** cvAvailableForSmoke;
pthread_mutex_t smokeLock;

///////// FUNCTIONS
/*
int signal(pthread_cond_t *cond){
    return pthread_cond_signal(cond);
}  
int wait(pthread_cond_t *cond, pthread_mutex_t *mutex){
    return pthread_cond_wait(cond,mutex);
} 
*/

int signal(sem_t* s){
    return sem_post(s);
}  

int wait(sem_t* s){
    return sem_wait(s);
} 

void printPrivate(Private* p){
    cerr << "GID: " << p->gid << endl;
    cerr << "si x sj: " << p->si << "x" << p->sj << endl;
    cerr << "t: " << p->tg << endl;
    cerr << "n: " << p->ng << endl;
    for(auto v : p->areas)
        cerr << "(" << v.first << "," << v.second << ")" << endl;
}

void printGrid(){
    for(int i=0;i<n;i++){ // cigbutt counts
        for(int j=0;j<m;j++){
            cerr << grid[i][j] << " ";
            if(grid[i][j]<10)
                cerr << " ";
        }
        cerr << endl;
    }
}

int waitCells(pair<int,int>& coord, int si, int sj, int gid){ 
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    bool checkAgain = true;

    
    while(checkAgain){
        checkAgain = false;
        // check cvAvailable: checking if all cells are free of gatherer
        pthread_mutex_lock(&availableLock);
        for(int i=coord.first;i<boundary_i;i++){
            for(int j=coord.second;j<boundary_j;j++){
                if(cvAvailable[i][j] == NULL)
                    continue;
                pthread_cond_wait(cvAvailable[i][j],&availableLock);// unlocks the lock before blocking, locks after returning
                
                pthread_mutex_lock(&breakLock);
                if(Stop){
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableLock);
                    hw2_notify(GATHERER_STOPPED,gid,0,0);
                    return 2;
                }
                else if(Break){
                    pthread_mutex_unlock(&breakLock);
                    pthread_mutex_unlock(&availableLock);
                    hw2_notify(GATHERER_TOOK_BREAK,gid,0,0);
                    pthread_mutex_lock(&contLock);
                    pthread_cond_wait(&cvCont,&contLock);
                    pthread_mutex_unlock(&contLock);
                    hw2_notify(GATHERER_CONTINUED, gid, 0 ,0);
                    return 1;
                }
                else
                    pthread_mutex_unlock(&breakLock);
                checkAgain = true;
                break;
            }
            if(checkAgain) break;
        }
        pthread_mutex_unlock(&availableLock);
        if(checkAgain) continue;
        // check cvAvailableForSmoke: checking if all cells are free of smoker
    }

    pthread_mutex_lock(&availableLock);
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            wait(&gridSem[i][j]);
            cvAvailable[i][j] = new pthread_cond_t;
            pthread_cond_init(cvAvailable[i][j],NULL);
        }
    }
    pthread_mutex_unlock(&availableLock);
    return 0;
}

void signalCells(pair<int,int>& coord, int si, int sj,int gid){ 
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    pthread_mutex_lock(&availableLock);
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            pthread_cond_broadcast(cvAvailable[i][j]);
            //cerr << "G" << gid << " signaled " << i << "x" << j << endl;
            signal(&gridSem[i][j]);
            pthread_cond_destroy(cvAvailable[i][j]);
            cvAvailable[i][j] = NULL;
            
        }
    }
    pthread_mutex_unlock(&availableLock);
}

int cleanArea(pair<int,int>& coord, int si, int sj, int tg, int gid){
    int boundary_i = coord.first+si < n ? coord.first+si : n ;
    int boundary_j = coord.second+sj < m ? coord.second+sj : m;
    int tw;
    for(int i=coord.first;i<boundary_i;i++){
        for(int j=coord.second;j<boundary_j;j++){
            while(grid[i][j] > 0){
                struct timeval current_time;// sleeping time calculations
                struct timespec waiting_time;
                gettimeofday(&current_time, NULL);
                current_time.tv_usec += tg*1000;
                waiting_time.tv_sec = current_time.tv_sec + current_time.tv_usec/1000000;
                waiting_time.tv_nsec = (current_time.tv_usec%1000000)*1000;

                pthread_mutex_lock(&breakLock);
                tw = pthread_cond_timedwait(&cvBreak,&breakLock,&waiting_time); // it releases the breakLock before blocking, acquires it before returning
                if(tw == ETIMEDOUT)// not on a break  
                    pthread_mutex_unlock(&breakLock);
                else if(Stop){// end the thread
                    pthread_mutex_unlock(&breakLock);
                    signalCells(coord,si,sj,gid); //unlock all cells
                    hw2_notify(GATHERER_STOPPED,gid,0,0);
                    return 2;
                }
                else if(Break){// take a break
                    pthread_mutex_unlock(&breakLock);
                    signalCells(coord,si,sj,gid); //unlock all cells
                    hw2_notify(GATHERER_TOOK_BREAK, gid, 0, 0);
                    pthread_mutex_lock(&contLock);
                    pthread_cond_wait(&cvCont,&contLock);
                    pthread_mutex_unlock(&contLock);
                    hw2_notify(GATHERER_CONTINUED, gid, 0 ,0);
                    return 1;
                }
                grid[i][j]--;
                hw2_notify(GATHERER_GATHERED, gid, i, j);
            }
        }
    }
    hw2_notify(GATHERER_CLEARED, gid, 0, 0);
    return 0;
}

void executeOrders(vector<pair<int,string>>& orders, pthread_t* tids){
    int time = 0;
    int t;
    ///////////////////////////FOR PART2
    for(auto o: orders){
        t = o.first - time;
        usleep(t*1000);
        time = o.first;
        if(!o.second.compare("break")){ // send break signal to all gatherers
            hw2_notify(ORDER_BREAK,0,0,0);
            pthread_mutex_lock(&breakLock);
            Break = 1;
            pthread_mutex_unlock(&breakLock);
            pthread_cond_broadcast(&cvBreak);
        }
        else if(!o.second.compare("continue")){ // send continue signal to all gatherers
            hw2_notify(ORDER_CONTINUE,0,0,0);
            pthread_mutex_lock(&breakLock);
            if(Break){
                pthread_cond_broadcast(&cvCont);
            }
            Break = 0;
            pthread_mutex_unlock(&breakLock);
        }
        else if(!o.second.compare("stop")){ // send stop signal to all gatherers
            hw2_notify(ORDER_STOP,0,0,0);
            pthread_mutex_lock(&breakLock);
            Break = 0;
            Stop = 1;
            pthread_mutex_unlock(&breakLock);
            pthread_cond_broadcast(&cvBreak);
        }
        else
            cerr << "INVALID ORDER" << endl;
    }
}

////////// THREADS

void *gatherer(void *arg){ //arguments: grid, private
    Private* p = (Private*) arg;
    int si = p->si;
    int sj = p->sj;
    int tg = p->tg;
    int restartReq = 1;
    int backupCoord=0;
    int size = p->areas.size();
    int c = 0;
    while(restartReq){
        restartReq = 0; // should be unnecessary
        c++;
        for(int coord=backupCoord;coord<size;coord++ ){ 
                //cerr << "G" << p->gid << " is waiting " <<  c << endl;
            // WAIT FOR ALL CELLS IN THE AREA
            restartReq = waitCells(p->areas[coord], si, sj,p->gid);
                //cerr << "G" << p->gid << " is finished waiting " <<  c << endl;
            if(restartReq==1){
                backupCoord =coord;
                break;
            }
            else if(restartReq==2){
                pthread_exit(NULL);
            }

            hw2_notify(GATHERER_ARRIVED, p->gid, p->areas[coord].first, p->areas[coord].second);
            
            // CLEAN AREA
            restartReq = cleanArea(p->areas[coord], si, sj, tg, p->gid);
            if(restartReq==1){             
                backupCoord =coord;
                break;
            }
            else if(restartReq==2){
                pthread_exit(NULL);
            }

            // SIGNAL FOR ALL CELLS IN THE AREA
            signalCells(p->areas[coord], si, sj, p->gid);
        }
    }
    
    hw2_notify(GATHERER_EXITED, p->gid, 0, 0);

    return NULL;
}

void *commander(void* arg){
    commanderInput* inp = (commanderInput*) arg;
    executeOrders(*(inp->orders),inp->tids);
    return NULL;
}

void *smoker(void *arg){
    Smoker* s = (Smoker*) arg;
    int go = 0;
    
    for(int coord = 0;coord < s->ns; coord++){
        int ik = s->smokeAreas[coord].ik;
        int jk = s->smokeAreas[coord].jk;
        int ck = s->smokeAreas[coord].ck;
        
        // WAIT CELLS

        //wait for gatherer or smoker
        int checkAgain = true;
        while(checkAgain){
            checkAgain = false;
            pthread_mutex_lock(&availableLock);
            for(int i=ik-1;i<=ik+1;i++){
                for(int j=jk-1;j<=jk+1;j++){
                    if(cvAvailable[i][j] == NULL)
                        continue;
                    pthread_cond_wait(cvAvailable[i][j],&availableLock);

                    // STOP ORDER 

                    pthread_mutex_lock(&breakLock);
                    if(Stop){
                        pthread_mutex_unlock(&breakLock);
                        pthread_mutex_unlock(&availableLock);
                        hw2_notify(SNEAKY_SMOKER_STOPPED,s->sid,0,0);
                        pthread_exit(NULL);
                    }
                    else
                        pthread_mutex_unlock(&breakLock);

                    checkAgain = true;
                    break;
                }
                if(checkAgain) break;
            }
            pthread_mutex_unlock(&availableLock);
        }

        // LOCK CELLS
        pthread_mutex_lock(&smokeLock);
        for(int i=ik-1;i<=ik+1;i++){
            for(int j=jk-1;j<=jk+1;j++){
                if(i==ik && j==jk){
                    wait(&gridSem[i][j]);
                    pthread_mutex_lock(&availableLock);
                    cvAvailable[i][j] = new pthread_cond_t;
                    pthread_cond_init(cvAvailable[i][j],NULL);
                    pthread_mutex_unlock(&availableLock);
                }
                else{
                    cvAvailableForSmoke[i][j] = new pthread_cond_t;
                    pthread_cond_init(cvAvailableForSmoke[i][j],NULL);
                }
            }
        }
        pthread_mutex_unlock(&smokeLock);

        hw2_notify(SNEAKY_SMOKER_ARRIVED,s->sid,ik,jk);

        // SMOKE
        int litter_i = ik-1;
        int litter_j = jk-1;
        int tw;
        while(ck>0){
            usleep(s->ts*1000);
            //tw = pthread_cond_timedwait()
            
            // STOP ORDER

            pthread_mutex_lock(&gridMutex);
            grid[litter_i][litter_j]++;
            pthread_mutex_unlock(&gridMutex);
            hw2_notify(SNEAKY_SMOKER_FLICKED,s->sid,litter_i,litter_j);
            ck--;
        }

        hw2_notify(SNEAKY_SMOKER_LEFT,s->sid,0,0);
        

        // SIGNAL CELLS
        pthread_mutex_lock(&smokeLock);
        for(int i=ik-1;i<=ik+1;i++){
            for(int j=jk-1;j<=jk+1;j++){
                if(i==ik && j==jk){
                    pthread_mutex_lock(&availableLock);
                    pthread_cond_broadcast(cvAvailable[i][j]);
                    signal(&gridSem[i][j]);
                    pthread_cond_destroy(cvAvailable[i][j]);
                    cvAvailable[i][j] = NULL;
                    pthread_mutex_unlock(&availableLock);
                }
                else{
                    pthread_cond_broadcast(cvAvailableForSmoke[i][j]);
                    pthread_cond_destroy(cvAvailableForSmoke[i][j]);
                    cvAvailableForSmoke[i][j] = NULL;
                }
            }
        }
        pthread_mutex_unlock(&smokeLock);
    }        


    return NULL;
}

int main(){
    /////////// PART-I

    /// input taking
    hw2_init_notifier();
    cin >> n >> m;
    grid = new int*[n];
    numOfGatherers = new int*[n];
    gridSem = new sem_t*[n];
    cvAvailable = new pthread_cond_t**[n];
    cvAvailableForSmoke = new pthread_cond_t**[n];
    for(int i=0;i<n;i++){ // Cigbutt counts
        grid[i] = new int[m];
        numOfGatherers[i] = new int[m];
        gridSem[i] = new sem_t[m];
        cvAvailable[i] = new pthread_cond_t*[m];
        cvAvailableForSmoke[i] = new pthread_cond_t*[m];
        for(int j=0;j<m;j++){
            cin >> grid[i][j];
            numOfGatherers[i][j] = 0;
            sem_init(&gridSem[i][j],0,1); 
            cvAvailable[i][j] = NULL;
            cvAvailableForSmoke[i][j] = NULL;
        }
    }
    
    int numberOfPrivates; // Number of proper privates
    cin >> numberOfPrivates;

    Private privates[numberOfPrivates]; // holds proper privates
    for(int i=0;i<numberOfPrivates;i++){ // Creation of proper privates
        cin >> privates[i].gid;
        cin >> privates[i].si;
        cin >> privates[i].sj;
        cin >> privates[i].tg;
        cin >> privates[i].ng;
        int x,y;
        for(int j=0;j<privates[i].ng;j++){
            cin >> x >> y;
            privates[i].areas.push_back(make_pair(x,y));
        }
    }

    // INPUT FOR PART II
    
    int numberOfOrders; // number of orders
    vector<pair<int,string>> orders; // holds ms-order pairs
    cin >> numberOfOrders; 

    for(int i=0;i<numberOfOrders;i++){ // taking orders
        int ms;
        string command;
        cin >> ms >> command;
        orders.push_back(make_pair(ms,command));
    }

    Break = 0;
    Stop = 0;

    // INPUT FOR PART-III

    int numberOfSmokers;
    cin >> numberOfSmokers;

    Smoker smokers[numberOfSmokers];
    for(int i=0;i<numberOfSmokers; i++){
        cin >> smokers[i].sid;
        cin >> smokers[i].ts;
        cin >> smokers[i].ns;
        int ik,jk,ck;
        for(int j=0;j<smokers[i].ns;j++){
            Triplet triplet;
            cin >> triplet.ik >> triplet.jk >> triplet.ck;
            smokers[i].smokeAreas.push_back(triplet);
        }
    }

    // START GATHERERS
    pthread_t tids[numberOfPrivates];
    for(int t=0;t<numberOfPrivates;t++){
        pthread_create(&tids[t],NULL,gatherer,(void*) &privates[t]); 
        hw2_notify(GATHERER_CREATED, privates[t].gid, 0, 0);
    }
    
    // PART-II
    
    pthread_t ctid;
    commanderInput inp;
    inp.orders = &orders;
    inp.tids = tids;

    // START COMMANDER
    pthread_create(&ctid,NULL,commander,(void*) &inp); 

    // START SMOKERS
    pthread_t stids[numberOfSmokers];
    for(int s=0;s<numberOfSmokers;s++){
        pthread_create(&stids[s],NULL, smoker, (void*) &smokers[s]);
        hw2_notify(SNEAKY_SMOKER_CREATED, smokers[s].sid,0,0);
    }

    // PART-III
    
    for(int t=0;t<numberOfPrivates;t++)
        pthread_join(tids[t],NULL);
    for(int s=0;s<numberOfSmokers;s++)
        pthread_join(stids[s],NULL);
    pthread_join(ctid,NULL);
    cerr << "--------------------GRID--------------------- " << endl;
    printGrid();

    cerr << "--------------------END-----------------------" << endl;
    return 0;
    
}