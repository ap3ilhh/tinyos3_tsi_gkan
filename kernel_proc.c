
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);

  /**********************************************/
  /*arxikopoihsh listas kai thread_count*/
  rlnode_init(&pcb->ptcb_list,NULL);
  pcb->thread_count = 0;

  pcb->child_exit = COND_INIT;
}

void initialize_PTCB(PTCB* ptcb)
{
  ptcb->argl = 0;
  ptcb->args = NULL;
  rlnode_init(&ptcb->ptcb_list_node,ptcb);

}



static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /******************************************************************/
  /*gia to multithread prepei na exw mia endiamesh domh PTCB*/

  /*pointer se ptcb kai malloc gia ena ptcb*/
  PTCB* ptcb;
  ptcb = (PTCB*)xmalloc(sizeof(PTCB));
  /*arxikopoihsh PTCB*/
  initialize_PTCB(ptcb); 


  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {
    newproc->main_thread = spawn_thread(newproc, start_main_thread);
    newproc->thread_count++;

    ptcb->tcb = newproc->main_thread;         //to ptcb deixnei sto pcb

    ptcb->tcb->ptcb = ptcb;                   //to tcb deixnei sto ptcb 

    ptcb->task = call;
    ptcb->argl = argl;
    ptcb->args = args;

    ptcb->exited = 0;
    ptcb->detached = 0;
    ptcb->exit_cv = COND_INIT; //???????????????

    ptcb->refcount = 0;

    //vazw sth process sth lista me ta ptcb to kainourio ptcb
    rlist_push_back(&newproc->ptcb_list,&ptcb->ptcb_list_node);

    wakeup(newproc->main_thread);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;

  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  if(get_pid(curproc)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

  }
  sys_ThreadExit(exitval);
}

static file_ops procinfo_ops = {
  .Open = null_procinfo_open,
  .Read = procinfo_read,
  .Write = null_procinfo_write,
  .Close = procinfo_close
};




void* null_procinfo_open(uint minor){
  return NULL;
}

int null_procinfo_write(void* procinfoCB_t, const char* buf, unsigned int size){
  return -1;
}

Fid_t sys_OpenInfo()
{
  Fid_t fid;
  FCB* fcb;

  if (FCB_reserve(1,&fid,&fcb) == 0){
    return NOFILE;
  }

  procinfo_cb* procinfoCB;
  procinfoCB = (procinfo_cb*)xmalloc(sizeof(procinfo_cb));

  procinfoCB->PCB_cursor = 1;

  fcb->streamobj = procinfoCB;
  fcb->streamfunc = & procinfo_ops;

  return fid;
}

int procinfo_close(void* procinfoCB_t)
{
  if (procinfoCB_t == NULL)
    return -1;
  free(procinfoCB_t);
  return 0;
}

int procinfo_read(void* procinfoCB_t, char *buf, unsigned int n)
{

  procinfo_cb* procinfoCB = (procinfo_cb*)procinfoCB_t;

  if (procinfoCB == NULL)
    return -1;
  /*oso to PCB->cursor deixne sta oria tou PT[]*/
  while(procinfoCB->PCB_cursor < MAX_PROC){
    /*an to process einai FREE proxwra*/
    if (PT[procinfoCB->PCB_cursor].pstate == FREE)
    {
      procinfoCB->PCB_cursor++;
    }
    /*to process einai alive h' zombie*/
    else
    {
      PCB proc = PT[procinfoCB->PCB_cursor];
      /*enhmerwsh procinfo*/
      procinfoCB->p_info.pid = get_pid(&PT[procinfoCB->PCB_cursor]);
      procinfoCB->p_info.ppid = get_pid(proc.parent);

      procinfoCB->p_info.alive = (proc.pstate == ALIVE) ? 1 : 0;

      procinfoCB->p_info.thread_count = proc.thread_count;

      procinfoCB->p_info.main_task = PT[procinfoCB->PCB_cursor].main_task;

      procinfoCB->p_info.argl =  proc.argl;
      /*an argl megalutero tou PROCINFO_MAX_ARGS_SIZE kanw memcpy PROCINFO_MAX_ARGS_SIZE bytes
        alliws kanw memcpy argl bytes*/
      int sizeof_args = (proc.argl > PROCINFO_MAX_ARGS_SIZE) ? PROCINFO_MAX_ARGS_SIZE :proc.argl ; 

      if (proc.args != NULL){
        memcpy(procinfoCB->p_info.args,proc.args,sizeof_args);
      }

      memcpy(buf,(char*)&procinfoCB->p_info,n);

      procinfoCB->PCB_cursor++;

      return n; 
    }     

    
  }
  /*EOF* afou kseperasa ta oria tou pinaka kai vghka apo th while*/
  return 0;
  
}