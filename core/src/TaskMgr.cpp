#include "TaskMgr.h"
#include "LinkTask.h"
#include "SinkTask.h"
#include "PoolThreadMgr.h"

//Class TaskMgr::TaskLinkWrap
class TaskLinkWrap : public TaskMgr::TaskWrap
{
public:
  TaskLinkWrap(TaskMgr &aMgr,LinkTask *aTask,Data &aCurrentData) : 
    TaskWrap(aMgr),_LinkTask(aTask),_currentData(aCurrentData) {}
  virtual ~TaskLinkWrap()
  {
    _endLinkTask(_LinkTask);
  }
  virtual void process()
  {
    TaskEventCallback* eventCbkPt = _LinkTask->getEventCallback();
    if(eventCbkPt)
      eventCbkPt->started(_currentData);
    Data aResult = _LinkTask->process(_currentData);
    if(eventCbkPt)
      eventCbkPt->finished(aResult);
    _setNextData(aResult);
  }
private:
  LinkTask	*_LinkTask;
  Data          _currentData;
};

//Class TaskSinkWrap
class TaskSinkWrap : public TaskMgr::TaskWrap
{
public:
  TaskSinkWrap(TaskMgr &aMgr,SinkTaskBase *aTask,Data &aCurrentData) : 
    TaskWrap(aMgr),_SinkTask(aTask),_currentData(aCurrentData) {}

  virtual ~TaskSinkWrap()
  {
    _endSinkTask(_SinkTask);
  }
  virtual void process()
  {
    TaskEventCallback* eventCbkPt = _SinkTask->getEventCallback();
    if(eventCbkPt)
      eventCbkPt->started(_currentData);
    _SinkTask->process(_currentData);
    if(eventCbkPt)
      eventCbkPt->finished(_currentData);
  }
private:
  SinkTaskBase	*_SinkTask;
  Data          _currentData;
};

//struct Task

TaskMgr::Task* TaskMgr::Task::copy() const
{
  TaskMgr::Task *aReturnTask = new TaskMgr::Task();
  if(_linkTask)
    {
      aReturnTask->_linkTask = _linkTask;
      _linkTask->ref();
    }
  for(std::deque<SinkTaskBase*>::const_iterator i = _sinkTaskQueue.begin();
      i != _sinkTaskQueue.end();++i)
    {
      (*i)->ref();
      aReturnTask->_sinkTaskQueue.push_back(*i);
    }
  return aReturnTask;
}

TaskMgr::Task::~Task()
{
  if(_linkTask)
    _linkTask->unref();
  for(std::deque<SinkTaskBase*>::iterator i = _sinkTaskQueue.begin();
      i != _sinkTaskQueue.end();++i)
    (*i)->unref();
}

TaskMgr::TaskMgr() : _PendingLinkTask(NULL),_initStageFlag(false) {}

TaskMgr::TaskMgr(const TaskMgr &aMgr)
{
  for(StageTask::const_iterator i = aMgr._Tasks.begin();
      i != aMgr._Tasks.end();++i)
    _Tasks.push_back((*i)->copy());
  _currentData = aMgr._currentData;
}

TaskMgr::~TaskMgr()
{
  for(StageTask::iterator i = _Tasks.begin();
      i != _Tasks.end();++i)
    delete *i;
}
/** @brief add a linked task
 *  @return true if task set for stage
 */
bool TaskMgr::setLinkTask(int aStage,LinkTask *aNewTask)
{
  bool aReturnFlag = false;
  while(int(_Tasks.size()) <= aStage)
    _Tasks.push_back(new Task());
  Task *aTaskPt = _Tasks[aStage];
  if(!aTaskPt->_linkTask)
    {
      aNewTask->ref();
      aTaskPt->_linkTask = aNewTask;
      if(!aTaskPt->_sinkTaskQueue.empty())
	aNewTask->setProcessingInPlace(false);
      aReturnFlag =  true;
    }
  
  return aReturnFlag;
}

void TaskMgr::addSinkTask(int aStage,SinkTaskBase *aNewTask)
{
  while(int(_Tasks.size()) <= aStage)
    _Tasks.push_back(new Task());
  aNewTask->ref();
  Task *aTaskPt = _Tasks[aStage];
  aTaskPt->_sinkTaskQueue.push_back(aNewTask);
  if(aTaskPt->_linkTask)
    aTaskPt->_linkTask->setProcessingInPlace(false);

}
void TaskMgr::getLastTask(std::pair<int,LinkTask*> &aLastLink,
			  std::pair<int,SinkTaskBase*> &aLastSink)
{
  aLastLink.first = -1,aLastLink.second = NULL;
  aLastSink.first = -1,aLastSink.second = NULL;

  for(int i = _Tasks.size() - 1;
      i >= 0 && aLastSink.first < 0 && aLastLink.first < 0;--i)
    {
      Task *aTaskPt = _Tasks[i];
      if(aLastLink.first < 0 && aTaskPt->_linkTask)
	{
	  aLastLink.first = i;
	  aLastLink.second = aTaskPt->_linkTask;
	}
      if(aLastSink.first < 0 && !aTaskPt->_sinkTaskQueue.empty())
	{
	  aLastSink.first = i;
	  aLastSink.second = aTaskPt->_sinkTaskQueue.back();
	}
    }
}

TaskMgr::TaskWrap* TaskMgr::next()
{
#define CHECK_END_STAGE()					\
  if(aTaskPt->_sinkTaskQueue.empty())				\
    {								\
      PoolThreadMgr::get().removeProcess(this,false);		\
      delete aTaskPt;						\
      _Tasks.pop_front();					\
    }								
  
  Task *aTaskPt = _Tasks.front();
  while(!aTaskPt->_linkTask && aTaskPt->_sinkTaskQueue.empty())
    {
      delete aTaskPt;
      _Tasks.pop_front();
      aTaskPt = _Tasks.front();
    }
  if(!_initStageFlag)
      _initStageFlag = true,_nbPendingSinkTask = aTaskPt->_sinkTaskQueue.size();

  if(aTaskPt->_linkTask)		// first linked task
    {
      _PendingLinkTask = aTaskPt->_linkTask;
      aTaskPt->_linkTask = NULL;
      CHECK_END_STAGE();
      return new TaskLinkWrap(*this,_PendingLinkTask,_currentData);
    }
  else
    {
      SinkTaskBase *aNewSinkTaskPt = aTaskPt->_sinkTaskQueue.front();
      aTaskPt->_sinkTaskQueue.pop_front();
      CHECK_END_STAGE();
      return new TaskSinkWrap(*this,aNewSinkTaskPt,_currentData);
    }
}

void TaskMgr::_endLinkTask(LinkTask*)
{
  if(_PendingLinkTask)
    {
      _PendingLinkTask->unref();
      _PendingLinkTask = NULL;
    }

  if(!_nbPendingSinkTask)
    _goToNextStage();
}

void TaskMgr::_endSinkTask(SinkTaskBase*)
{
  --_nbPendingSinkTask;
  if(!_PendingLinkTask && !_nbPendingSinkTask)
      _goToNextStage();
}

//@brief Processing Stage finished, going to next stage
void TaskMgr::_goToNextStage()
{
  _currentData = _nextData;	// swap
  _nextData.releaseBuffer();
  if(_Tasks.empty())	// Process is finished
    delete this;		// suicide
  else
    {
      _initStageFlag = false;
      PoolThreadMgr::get().addProcess(this,false); // Re insert in the Poll
    }
}

void TaskMgr::syncProcess()
{
  for(StageTask::iterator aStageTask = _Tasks.begin();
      aStageTask != _Tasks.end();++aStageTask)
    {
      Task *aStageTaskPt = *aStageTask;
      if(aStageTaskPt->_linkTask)
	{
	  _nextData = aStageTaskPt->_linkTask->process(_currentData);
	  aStageTaskPt->_linkTask->unref();
	  aStageTaskPt->_linkTask = NULL;
	}
      for(std::deque<SinkTaskBase*>::iterator aSinkTask = aStageTaskPt->_sinkTaskQueue.begin();
	  aSinkTask != aStageTaskPt->_sinkTaskQueue.end();++aSinkTask)
	{
	  (*aSinkTask)->process(_currentData);
	  (*aSinkTask)->unref();
	}
      aStageTaskPt->_sinkTaskQueue.clear();
      if(!_nextData.empty())
	{
	  _currentData = _nextData; // swap
	  _nextData.releaseBuffer();
	}
      delete aStageTaskPt;
    }
  _Tasks.clear();
}
