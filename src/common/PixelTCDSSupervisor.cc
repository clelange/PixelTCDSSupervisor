#include "PixelTCDSSupervisor/PixelTCDSSupervisor.h"
#include "PixelTCDSSupervisor/exception/Exception.h"

#include <cassert>
#include <list>
#include <stdint.h>
#include <string>
#include <vector>
#include <fstream>
#include <streambuf>

#include "log4cplus/logger.h"
#include "log4cplus/loggingmacros.h"
#include "xcept/Exception.h"
#include "xdaq/ApplicationDescriptor.h"
#include "xdaq/ContextDescriptor.h"
#include "xdaq/exception/ApplicationInstantiationFailed.h"
#include "xgi/Input.h"
#include "xgi/Output.h"
#include "xoap/SOAPElement.h"
#include "cgicc/HTMLClasses.h"

#include "xgi/framework/Method.h"
#include "xdaq/NamespaceURI.h"
#include "xoap/Method.h"
#include "xoap/MessageFactory.h"
#include "xoap/SOAPPart.h"
#include "xoap/SOAPEnvelope.h"
#include "xoap/SOAPBody.h"
#include "xoap/domutils.h"



XDAQ_INSTANTIATOR_IMPL(pixel::tcds::PixelTCDSSupervisor);

pixel::tcds::PixelTCDSSupervisor::PixelTCDSSupervisor(xdaq::ApplicationStub* stub)
try
  :
    xdaq::Application(stub),
    xgi::framework::UIManager(this),
    PixelTCDSBase(this),
    hwCfgString_("# Dummy hardware configuration string"),
    hwCfgFileName_("/nfshome0/clange/TCDSConfigurations/pixel_tcds_config_v4.txt"),
    receivedCfgString_(false),
    runNumber_(42),
    tcdsState_("uninitialised"),
    tcdsHwLeaseOwnerId_("uninitialised"),
    statusMsg_(""),
    logger_(getApplicationLogger())
  {
	timeStart_= toolbox::TimeVal::gettimeofday();
    // Set the application icon.
    std::string const iconFileName = "/pixel/PixelWeb/icons/pixelici_icon.png";
    getApplicationDescriptor()->setAttribute("icon", iconFileName);

    // Registration of the InfoSpace variables.
    getApplicationInfoSpace()->fireItemAvailable("stateName",
                                                 &tcdsState_);
    getApplicationInfoSpace()->fireItemAvailable("hardwareConfigurationString",
                                                 &hwCfgString_);
    getApplicationInfoSpace()->fireItemAvailable("hardwareConfigurationFile",
                                                 &hwCfgFileName_);
    getApplicationInfoSpace()->fireItemAvailable("runNumber",
                                                 &runNumber_);
    std::ostringstream oss;
    oss << getApplicationDescriptor()->getClassName() << getApplicationDescriptor()->getInstance();
	appNameAndInstance_ = oss.str();
	std::ostringstream oss2;
    oss2 << getApplicationDescriptor()->getClassName() << " instance " <<getApplicationDescriptor()->getInstance();
	appNamePlusInstance_= oss2.str();
    // Binding of the main HyperDAQ page.
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::mainPage, "Default");

    // Binding of the remote queries.
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::queryFSMState, "QueryFSMState");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::queryFSMState, "FSMStateRequest");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::queryHwLeaseOwner, "QueryHardwareLeaseOwnerId");

    // Binding of the various web entry points to the state machine
    // transitions.
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::reset, "Reset");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::initialize, "Initialize");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::configure, "Configure");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::enable, "Start");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::pause, "Pause");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::resume, "Resume");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::stop, "Stop");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::halt, "Halt");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::coldReset, "ColdReset");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::ttcResync, "TTCResync");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::ttcHardReset, "TTCHardReset");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::renewHardwareLease, "RenewHardwareLease");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::readHardwareConfiguration, "ReadHardwareConfiguration");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::sendL1A, "SendL1A");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::sendBgo, "SendBgo");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::sendBgoString, "SendBgoString");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::sendBgoTrain, "SendBgoTrain");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::updateHardwareConfigurationFile, "UpdateHardwareConfigurationFile");
    xgi::framework::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::updateHardwareConfiguration, "UpdateHardwareConfiguration");
	xgi::deferredbind(this, this, &pixel::tcds::PixelTCDSSupervisor::jsonUpdate, "update");

    // Bind workloop function
    job_ = toolbox::task::bind(this, &pixel::tcds::PixelTCDSSupervisor::working, "working");
    workLoop_ = toolbox::task::getWorkLoopFactory()->getWorkLoop(appNameAndInstance_, "waiting");

    // SOAP bindings
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "QueryFSMState", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "FSMStateRequest", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Handshake", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "ReceiveConfigString", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Reset", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Initialize", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Configure", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Start", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Pause", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Suspend", XDAQ_NS_URI ); // same as Pause for backward compatibility
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Resume", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Stop", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "Halt", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "ColdReset", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "TTCResync", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "TTCHardReset", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "RenewHardwareLease", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "ReadHardwareConfiguration", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "SendL1A", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "UpdateHardwareConfigurationFile", XDAQ_NS_URI );
    xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "UpdateHardwareConfiguration", XDAQ_NS_URI );
	xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "update", XDAQ_NS_URI );
    // xoap::bind(this, &pixel::tcds::PixelTCDSSupervisor::fireEvent, "SendBgo", XDAQ_NS_URI );

    // Define FSM
    fsm_.addState ('I', "Initial", this, &pixel::tcds::PixelTCDSSupervisor::stateChanged);
    fsm_.addState ('H', "Halted", this, &pixel::tcds::PixelTCDSSupervisor::stateChanged);
    fsm_.addState ('c', "Configuring", this, &pixel::tcds::PixelTCDSSupervisor::stateChanged);
    fsm_.addState ('C', "Configured", this, &pixel::tcds::PixelTCDSSupervisor::stateChanged);
    fsm_.addState ('R', "Running", this, &pixel::tcds::PixelTCDSSupervisor::stateChanged);
    fsm_.addState ('P', "Paused", this, &pixel::tcds::PixelTCDSSupervisor::stateChanged);
    // fsm_.addState ('S', "Stopped");

    fsm_.addStateTransition('I','H', "Initialize"); //, this, &PixelTTCSupervisor::ConfigureAction);
    fsm_.addStateTransition('H', 'c', "Configure"); //, this, &PixelSupervisor::transitionHaltedToConfiguring);
    fsm_.addStateTransition('c', 'c', "Configure");
    fsm_.addStateTransition('c', 'C', "ConfiguringDone");
    fsm_.addStateTransition('C', 'R', "Start");
    fsm_.addStateTransition('R', 'C', "Stop");
    fsm_.addStateTransition('R', 'P', "Pause");
    fsm_.addStateTransition('P', 'R', "Resume");
    fsm_.addStateTransition('P', 'C', "Stop");
    fsm_.addStateTransition('C', 'H', "Halt");
    fsm_.addStateTransition('P', 'H', "Halt");
    fsm_.addStateTransition('R', 'H', "Halt");
    // fsm_.addStateTransition('S', 'H', "Halt");
    fsm_.addStateTransition('H', 'H', "ColdReset");
    fsm_.addStateTransition('F', 'H', "Recover");
    // define error state
    fsm_.setStateName('F',"Error");

    fsm_.setInitialState('I');
    fsm_.reset();

    // make sure no PixelSupervisor remains
    PixelSupervisor_ = 0;
    firstTransition = true;


  }
catch (xcept::Exception const& err)
  {
    std::string const msgBase = "Something went wrong instantiating your PixeliCISupervisor";
    std::string const msg = toolbox::toString("%s: '%s'.", msgBase.c_str(), err.what());
    XCEPT_RAISE(xdaq::exception::ApplicationInstantiationFailed, msg.c_str());
  }

pixel::tcds::PixelTCDSSupervisor::~PixelTCDSSupervisor()
{
}

/**
 FSM related functions
**/

// FSM state transition
void pixel::tcds::PixelTCDSSupervisor::fsmTransition(std::string transitionName)
  throw (xcept::Exception)
{
  try
  {
    toolbox::Event::Reference e(new toolbox::Event(transitionName,this));
  	fsm_.fireEvent(e);
  }
  catch (toolbox::fsm::exception::Exception & e)
  {
    XCEPT_RETHROW(xcept::Exception, "invalid state transition", e);
  }
}

void pixel::tcds::PixelTCDSSupervisor::stateChanged(toolbox::fsm::FiniteStateMachine & fsm) throw (toolbox::fsm::exception::Exception)
{
  // Reflect the new state
  std::string state = fsm_.getStateName (fsm_.getCurrentState());
  LOG4CPLUS_INFO (getApplicationLogger(), "New state is: " << state );

  //  if (!firstTransition && state == "Configured") {
  if (!firstTransition) {
    // try {
      //if (PixelSupervisor_!=0) {
        //Attribute_Vector parameters(3);
        //parameters[0].name_="Supervisor"; parameters[0].value_="PixeliCISupervisor";
        //parameters[1].name_="Instance";   parameters[1].value_=itoa(getApplicationDescriptor()->getInstance());
        //parameters[2].name_="FSMState";   parameters[2].value_=state;
	//LOG4CPLUS_INFO (getApplicationLogger(), "Sending state " << state << " to PixelSupervisor.");
	//std::string reply = Send(PixelSupervisor_, "FSMStateNotification", parameters);
	//LOG4CPLUS_INFO (getApplicationLogger(), "Received reply: " << reply);
      //}
    //}
    // catch (xcept::Exception & ex) {
    //  LOG4CPLUS_ERROR (getApplicationLogger(), "Failed to report FSM state " << state << " to PixelSupervisor. Exception: " << ex.what());
    // }
  }
  else {
    firstTransition = false;
  }

  if (state == "Configuring")
    {
      // submit the job in working state
      try
        {
          workLoop_->submit(job_);
          toolbox::Event::Reference e(new toolbox::Event("ConfiguringDone",this));
          fsm_.fireEvent(e);
        }
      catch (xdaq::exception::Exception& e)
        {
          LOG4CPLUS_ERROR (getApplicationLogger(), xcept::stdformat_exception_history(e));
          this->notifyQualified("error",e);
        }
    }
  else if (state == "Configured")
    {
      LOG4CPLUS_INFO (getApplicationLogger(), "Finished configuring");
    }
}

bool pixel::tcds::PixelTCDSSupervisor::working(toolbox::task::WorkLoop* wl)
{
  LOG4CPLUS_INFO(getApplicationLogger(), "Waiting for ICIController to configure...");
  // query here if ICIController is in correct state
  ::sleep(1);
  queryFSMStateAction();
  if (tcdsState_ == "Configured") {
    // Return false to return to terminate job
    LOG4CPLUS_INFO(getApplicationLogger(), "Finished configuring ICIController!");
    return false;
  }
  LOG4CPLUS_INFO(getApplicationLogger(), "Not yet configured: " << tcdsState_.toString() );

	// return true to automatically re-submit the function to the Workloop
	return true;
}

/**
 actions for xgi and soap bindings to avoid code duplication
**/

void
pixel::tcds::PixelTCDSSupervisor::queryFSMStateAction(bool explicitCall)
{
  if (explicitCall)
    statusMsg_ = "QueryFSMState";
  try
    {
      tcdsState_ = tcdsQueryFSMState();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::queryHwLeaseOwnerAction(bool explicitCall)
{
  if (explicitCall)
    statusMsg_ = "QueryHwLeaseOwner";
  try
    {
      tcdsHwLeaseOwnerId_ = tcdsQueryHwLeaseOwnerId();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::resetAction()
{
  statusMsg_ = "Reset";
  try
    {
      fsm_.reset();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;

    }
}

void
pixel::tcds::PixelTCDSSupervisor::initializeAction()
{
  statusMsg_ = "Initialize";
  // Detect PixelSupervisor
  try {
    PixelSupervisor_=getApplicationContext()->getDefaultZone()->getApplicationGroup("daq")->getApplicationDescriptor("PixelSupervisor", 0);
    LOG4CPLUS_INFO(getApplicationLogger(), "Found PixelSupervisor!");
	} catch (xdaq::exception::Exception& e) {
	  PixelSupervisor_=0;
    LOG4CPLUS_ERROR(getApplicationLogger(), "PixelSupervisor not found!");
	}

  try
    {
      tcdsHalt();
      if (!workLoop_->isActive()) {
        workLoop_->activate();
      }
      fsmTransition("Initialize");
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
    }
}

void
pixel::tcds::PixelTCDSSupervisor::configureAction()
{
  statusMsg_ = "Configure";
  try
    {
      if (!receivedCfgString_)
        readConfigFile();
      tcdsConfigure(hwCfgString_.toString());
      fsmTransition("Configure");
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::enableAction()
{
  statusMsg_ = "Start";
  try
    {
      tcdsEnable((unsigned int)(runNumber_));
      fsmTransition("Start");
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::pauseAction()
{
  statusMsg_ = "Pause";
  try
    {
      tcdsPause();
      fsmTransition("Pause");
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::resumeAction()
{
  statusMsg_ = "Resume";
  try
    {
      tcdsResume();
      fsmTransition("Resume");
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::stopAction()
{
  statusMsg_ = "Stop";
  try
    {
      tcdsStop();
      fsmTransition("Stop");
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::haltAction()
{
  statusMsg_ = "Halt";
  try
    {
      tcdsHalt();
      fsmTransition("Halt");
      receivedCfgString_=false;
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::coldResetAction()
{
  statusMsg_ = "ColdReset";
  try
    {
      tcdsColdReset();
      fsmTransition("ColdReset");
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::ttcResyncAction()
{
  statusMsg_ = "TTCResync";
  try
    {
      tcdsTTCResync();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::ttcHardResetAction()
{
  statusMsg_ = "TTCHardReset";
  try
    {
      tcdsTTCHardReset();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::renewHardwareLeaseAction()
{
  statusMsg_ = "RenewHardwareLease";
  try
    {
      tcdsRenewHardwareLease();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::readHardwareConfigurationAction()
{
  statusMsg_ = "ReadHardwareConfiguration";
  try
    {
      hwCfgString_ = tcdsReadHardwareConfiguration();
      // print configuration
      LOG4CPLUS_INFO(logger_, "Currrent Hardware Configuration:");
      LOG4CPLUS_INFO(logger_, hwCfgString_.toString());
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::sendL1AAction()
{
  statusMsg_ = "SendL1A";
  try
    {
      tcdsSendL1A();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::sendBgoAction(xdata::UnsignedInteger commandUInt)
{
  statusMsg_ = "SendBgo";
  try
    {
      tcdsSendBgo(commandUInt);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::sendBgoStringAction(xdata::String commandString)
{
  statusMsg_ = "SendBgoString";
  try
    {
      tcdsSendBgoString(commandString);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::sendBgoTrainAction(xdata::String trainString)
{
  statusMsg_ = "SendBgoTrain";
  try
    {
      tcdsSendBgoTrain(trainString);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}

void
pixel::tcds::PixelTCDSSupervisor::enableRandomTriggersAction(xdata::UnsignedInteger frequencyUInt)
{
  statusMsg_ = "EnableRandomTriggers";
  try
    {
      tcdsEnableRandomTriggers(frequencyUInt);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
}


/**
 xgi bindings
**/
int tam = 0;
void
pixel::tcds::PixelTCDSSupervisor::mainPage(xgi::Input* in, xgi::Output* out)
{
  // Local parameters (i.e., of this XDAQ application).
  //*out << "<h1>Local XDAQ: information</h1>\n\n";
 
	  
  loadWaitScreen(out);
  lostConnection(out);
  
	*out<<"<link href=\"/pixel/PixelWeb/css/bootstrap.css\" rel=\"stylesheet\">\n\n";
	*out<<"<script type=\"text/javascript\" src=\"/pixel/PixelWeb/js/bootstrap.js\"></script>\n";


	*out<<"<script type=\"text/javascript\">\n"
	  <<"$(window).onload = loadWin();\n"
	  <<"</script>\n\n";

  tabPresentation(out);
  tableSOAP(out);
}

void 
pixel::tcds::PixelTCDSSupervisor::printLine(xgi::Output* out, std::string nameText, std::string nameID){
	std::string idString = "<td id=\"" + nameID + "\">";
	
	*out<<"<tr>\n"
        <<"<td>"<<nameText<<"</td>\n"
        <<idString<< (nameID)<<"</td>\n"
		<<"</tr>\n";
} 

void
pixel::tcds::PixelTCDSSupervisor::redirect(xgi::Input* in, xgi::Output* out)
{
  // just for convenience update the remote information
  queryFSMStateAction();
  queryHwLeaseOwnerAction();
}

void
pixel::tcds::PixelTCDSSupervisor::loadWaitScreen(xgi::Output* out)
{

*out<<"<link href=\"/pixel/PixelWeb/css/please-wait.css\" rel=\"stylesheet\">\n";
*out<<"<link href=\"/pixel/PixelWeb/css/spinkit.css\" rel=\"stylesheet\">\n\n";

*out<<"<script type=\"text/javascript\" src=\"/pixel/PixelWeb/js/please-wait.js\"></script>\n";

  *out<<"<script type=\"text/javascript\">\n"
      <<"window.loading_screen = window.pleaseWait({\n"
      <<"logo: \"/pixel/PixelWeb/icons/pixelici_icon.png\",\n"
      <<"backgroundColor: '#f46d3b',\n"
	  <<"loadingHtml: \"<p class='loading-message'><font color='white' size='4'>Loading "<<appNamePlusInstance_<<"</font><div class=\'sk-rotating-plane\'></div>\"\n"
      <<"});\n"
	  <<"$(document).ready(loading_screen.finish());\n"
	  <<"</script>\n\n";
}

void
pixel::tcds::PixelTCDSSupervisor::lostConnection(xgi::Output* out)
{
	*out<<"<link href=\"/pixel/PixelWeb/css/xdaqPage.css\" rel=\"stylesheet\">\n\n";
	*out<<"<script type=\"text/javascript\" src=\"/pixel/PixelWeb/js/lostConnection.js\"></script>\n";
	*out<<"<script type=\"text/javascript\" src=\"/pixel/PixelWeb/js/windowLoad.js\"></script>\n";
	*out<<"<script type=\"text/javascript\">\n"
	  <<"$(window).onload = loadWin();\n"
	  <<"</script>\n\n";
}

void
pixel::tcds::PixelTCDSSupervisor::tableConfig(xgi::Output* out)
{
	*out<<"<div style=\"float:left;\">\n";
	*out<<"<h3>CONFIGURATION</h3>\n"
		<<"<p>Application Configuration.</p>\n";
	*out<<"<table class=\"table table-hover\" style=\"display: inline-block; float: left; width: 600px\">\n"
		<<"<thead>\n"
		<<"<tr>\n"
        <<"<th>Setting</th>\n"
        <<"<th>Status</th>\n"
		<<"</tr>\n"
		<<"</thead>\n"
		<<"<tbody>\n";
		printLine(out, "FSM state", "tb_Config_state");
		printLine(out, "Connected to TCDS application", "tb_Config_TCDS");
		printLine(out, "tb_Config_sessionID", "tb_Config_sessionID");
		printLine(out, "Hardware lease renewal interval", "tb_Config_renewInteval");
		printLine(out, "Run number", "tb_Config_runNumber");
		printLine(out, "Hardware configuration file", "tb_Config_hardware");
		printLine(out, "Last action", "tb_Config_statusMsg");

	*out<<"</tbody>\n"
		<<"</table>\n\n";
	*out<<"</div>\n";
}



void
pixel::tcds::PixelTCDSSupervisor::tableHistory(xgi::Output* out)
{
	*out<<"<table class=\"table table-hover\">\n"
		<<"<thead>\n"
		<<"<tr>\n"
        <<"<th>Timestamp</th>\n"
        <<"<th>Message</th>\n"
		<<"</tr>\n"
		<<"</thead>\n"
		<<"<tbody>\n"

		<<"<tr>\n"
        <<"<td>abc</td>\n"
        <<"<td>xyz</td>\n"
		<<"</tr>\n"

		<<"</tbody>\n"
		<<"</table>\n\n";
}

void
pixel::tcds::PixelTCDSSupervisor::tableSOAP(xgi::Output* out)
{
  *out<<"<script type=\"text/javascript\" src=\"/pixel/PixelWeb/js/button.js\"></script>\n";
  // A list of all available (remote) SOAP commands.
  *out << "<h3>SOAP commands</h3>";

  std::string const url(getApplicationDescriptor()->getContextDescriptor()->getURL());
  std::string const urn(getApplicationDescriptor()->getURN());

  std::vector<std::string> commands;
  commands.push_back("Initialize");
  commands.push_back("Configure");
  commands.push_back("Start");
  commands.push_back("Pause");
  commands.push_back("Resume");
  commands.push_back("Stop");
  commands.push_back("Halt");
  commands.push_back("ColdReset");
  commands.push_back("Reset");
  
  std::vector<std::string> commands2;
  commands2.push_back("TTCResync");
  commands2.push_back("TTCHardReset");
  commands2.push_back("RenewHardwareLease");
  commands2.push_back("ReadHardwareConfiguration");
  commands2.push_back("SendL1A");

  *out << "<ul>\n";

  *out << "<button href=\""
       << url << "/" << urn << "/QueryFSMState"
       << "\" class=\"btn btn-info\" role=\"button\" id=\"QueryFSMState\" onclick=\"buttonClick(this.id)\">Query FSM state</button>\n";

  *out << "<button href=\""
       << url << "/" << urn << "/QueryHardwareLeaseOwnerId"
       << "\" class=\"btn btn-info\" style=\"margin-left:20px\" role=\"button\" id=\"QueryHardwareLeaseOwnerId\" onclick=\"buttonClick(this.id)\">Query hardware lease owner</button>\n";
  *out << "</ul>\n";

  *out << "<ul>\n";
  for (std::vector<std::string>::const_iterator cmd = commands.begin();
       cmd != commands.end();
       ++cmd)
    {
      *out << "<button href=\""
           << url << "/" << urn << "/" << *cmd
           << "\" class=\"btn btn-primary\" id=\""<<*cmd<<"\" onclick=\"buttonClick(this.id)\" style=\"margin-left:10px; margin-top:20px\" type=\"button\" >" << *cmd << "</button>\n";
    }
  *out << "</ul>\n";
  
  *out << "<ul>\n";
  for (std::vector<std::string>::const_iterator cmd = commands2.begin();
       cmd != commands2.end();
       ++cmd)
    {
      *out << "<button href=\""
           << url << "/" << urn << "/" << *cmd
           << "\" class=\"btn btn-info\" id=\""<<*cmd<<"\" onclick=\"buttonClick(this.id)\" style=\"margin-left:10px; margin-top:20px\" type=\"button\" >" << *cmd << "</button>\n";
    }
  *out << "</ul>\n";

  *out<<"<button id=\"ExpertActions\" type=\"button\" class=\"btn btn-success active\" onclick=\"buttonClick(this.id)\">Expert Actions</button>\n";

  *out<<"<script type=\"text/javascript\">\n"
	  <<"defaultState();\n"
	  <<"</script>\n\n";
}


void
pixel::tcds::PixelTCDSSupervisor::tableBgoString(xgi::Output* out)
{
  std::string const url(getApplicationDescriptor()->getContextDescriptor()->getURL());
  std::string const urn(getApplicationDescriptor()->getURN());

//----------
  std::vector<std::string> bgoCommands;
  bgoCommands.push_back("Bgo0");
  bgoCommands.push_back("BC0");
  bgoCommands.push_back("TestEnable");
  bgoCommands.push_back("PrivateGap");
  bgoCommands.push_back("PrivateOrbit");
  bgoCommands.push_back("Resync");
  bgoCommands.push_back("HardReset");
  bgoCommands.push_back("EC0");
  bgoCommands.push_back("OC0");
  bgoCommands.push_back("Start");
  bgoCommands.push_back("Stop");
  bgoCommands.push_back("StartOfGap");
  bgoCommands.push_back("Bgo12");
  bgoCommands.push_back("WarningTestEnable");
  bgoCommands.push_back("Bgo14");
  bgoCommands.push_back("Bgo15");

  std::vector<std::string> bgoTrains;
  bgoTrains.push_back("Start");
  bgoTrains.push_back("Stop");
  bgoTrains.push_back("Pause");
  bgoTrains.push_back("Resume");
  bgoTrains.push_back("TTCResync");
  bgoTrains.push_back("TTCHardReset");
  // form for B-go string sending
  std::string sendBgoStringMethod =
            toolbox::toString("/%s/SendBgoString",urn.c_str());
  *out << cgicc::fieldset().set("style","font-size: 10pt; font-family: arial;");
          *out << std::endl;
          *out << cgicc::legend("Set B-go string to send") << cgicc::p() << std::endl;
  *out << cgicc::form().set("method","GET").set("action", sendBgoStringMethod) << std::endl;
  *out << "<select name=\"CommandString\">" << std::endl;
  for (unsigned int i = 0; i < bgoCommands.size(); ++i) {
    *out << "<option value=\"" << bgoCommands.at(i) << "\">" << bgoCommands.at(i) << std::endl;
  }
  *out << cgicc::input().set("type","submit").set("value","Send") << std::endl; *out << cgicc::form() << std::endl;
  *out << cgicc::fieldset();

  // form for B-go train sending
  std::string sendBgoTrainMethod =
            toolbox::toString("/%s/SendBgoTrain",urn.c_str());
  *out << cgicc::fieldset().set("style","font-size: 10pt; font-family: arial;");
          *out << std::endl;
          *out << cgicc::legend("Set B-go train string to send") << cgicc::p() << std::endl;
  *out << cgicc::form().set("method","GET").set("action", sendBgoTrainMethod) << std::endl;
  *out << "<select name=\"TrainString\">" << std::endl;
  for (unsigned int i = 0; i < bgoTrains.size(); ++i) {
    *out << "<option value=\"" << bgoTrains.at(i) << "\">" << bgoTrains.at(i) << std::endl;
  }
  *out << cgicc::input().set("type","submit").set("value","Send") << std::endl; *out << cgicc::form() << std::endl;
  *out << cgicc::fieldset();

  // form for B-go int sending
  std::string sendBgoMethod =
            toolbox::toString("/%s/SendBgo",urn.c_str());
  *out << cgicc::fieldset().set("style","font-size: 10pt; font-family: arial;");
          *out << std::endl;
          *out << cgicc::legend("Set B-go unsigned integer to send") << cgicc::p() << std::endl;
  *out << cgicc::form().set("method","GET").set("action", sendBgoMethod) << std::endl;
  *out << "<select name=\"CommandUInt\">" << std::endl;
  for (unsigned int bgoNumber = 0; bgoNumber <= 15; ++bgoNumber) {
    *out << "<option value=\"" << bgoNumber << "\">" << bgoNumber << std::endl;
  }
  *out << cgicc::input().set("type","submit").set("value","Send") << std::endl; *out << cgicc::form() << std::endl;
  *out << cgicc::fieldset();

  // form for EnableRandomTriggers uint sending
  std::string enableRandomTriggersMethod =
            toolbox::toString("/%s/EnableRandomTriggers",urn.c_str());
  *out << cgicc::fieldset().set("style","font-size: 10pt; font-family: arial;");
          *out << std::endl;
          *out << cgicc::legend("Set frequency at which to enable random triggers in Hz") << cgicc::p() << std::endl;
  *out << cgicc::form().set("method","GET").set("action", enableRandomTriggersMethod) << std::endl;
  *out << "<select name=\"FrequencyUInt\">" << std::endl;
  *out << "<option value=\"" << "100" << "\">" << "100" << std::endl;
  *out << cgicc::input().set("type","submit").set("value","Send") << std::endl; *out << cgicc::form() << std::endl;
  *out << cgicc::fieldset();

  // form for UpdateHardwareConfigurationFile
  std::string updateHardwareConfigurationFileMethod =
            toolbox::toString("/%s/UpdateHardwareConfigurationFile",urn.c_str());
  *out << cgicc::fieldset().set("style","font-size: 10pt; font-family: arial;");
          *out << std::endl;
          *out << cgicc::legend("Load new hardware configuration file (from disk accessible from XDAQ application)") << cgicc::p() << std::endl;
  *out << cgicc::form().set("method","GET").set("action", updateHardwareConfigurationFileMethod) << std::endl;
  *out << cgicc::label("&nbsp;&nbsp;&nbsp;&nbsp;(enter absolute path): ") << std::endl;
  *out << cgicc::input().set("type","text")
          .set("name", "FileNameString")
          .set("size", "100").set("value",hwCfgFileName_) << std::endl;
  *out << cgicc::input().set("type","submit").set("value","aaa") << std::endl; *out << cgicc::form() << std::endl;
  *out << cgicc::fieldset();

  // form for UpdateHardwareConfiguration
  // take file on local computer and upload it as config
  std::string updateHardwareConfigurationMethod =
            toolbox::toString("/%s/UpdateHardwareConfiguration",urn.c_str());
  *out << cgicc::fieldset().set("style","font-size: 10pt; font-family: arial;");
          *out << std::endl;
          *out << cgicc::legend("Load new hardware configuration (by uploading configuration file)") << cgicc::p() << std::endl;
  *out << cgicc::form().set("method","POST").set("action", updateHardwareConfigurationMethod).set("enctype", "multipart/form-data") << std::endl;
  *out << cgicc::input().set("type","file")
          .set("accept", "text/*")
          .set("name", "ConfigurationString") << std::endl;
  *out << cgicc::input().set("type","submit").set("value","Send") << std::endl; *out << cgicc::form() << std::endl;
  *out << cgicc::fieldset();

}

void
pixel::tcds::PixelTCDSSupervisor::tableStatus(xgi::Output* out)
{
*out<<"<table class=\"table table-hover\" style=\"display: inline-block; float: left;\">\n"
		<<"<thead>\n"
		<<"<tr>\n"
        <<"<th>Setting</th>\n"
        <<"<th>Status</th>\n"
		<<"</tr>\n"
		<<"</thead>\n"
		<<"<tbody>\n";

		printLine(out, "Application FSM state", "tb_Status_appFSM");
		printLine(out, "Application Status", "tb_Status_appstatus");
		printLine(out, "Problem Description", "tb_Status_prodesc");
		printLine(out, "RunControl session in charge", "tb_Status_runsession");
		printLine(out, "Application mode", "tb_Status_appmode");
		printLine(out, "Uptime", "tb_Status_uptime");
		printLine(out, "Latest monitoring update time", "tb_Status_timenow");
		printLine(out, "Latest monitoring update durations", "tb_Status_latestMonitoringDuration");
		
	*out<<"</tbody>\n"
		<<"</table>\n\n";
}

void
pixel::tcds::PixelTCDSSupervisor::tableRemoteInfo(xgi::Output* out)
{
// Remote parameters (i.e., of the remote TCDS control application).
*out<<"<div style=\"float:left; margin-left:50px\">\n";
*out<< "<h3>REMOTE INFOMATION</h3>\n"
	<<"<p>Application Remote Infomation</p>\n";
*out<<"<table class=\"table table-hover\" style=\"display: inline-block;\">\n"
		<<"<thead>\n"
		<<"<tr>\n"
        <<"<th>Setting</th>\n"
        <<"<th>Status</th>\n"
		<<"</tr>\n"
		<<"</thead>\n"
		<<"<tbody>\n";

		printLine(out, "State", "tb_Remote_tcdsState");
		printLine(out, "Hardware lease owner id", "tb_Remote_Hardware");

	*out<<"</tbody>\n"
		<<"</table>\n\n"
		<<"</div>\n";
}

void
pixel::tcds::PixelTCDSSupervisor::tableLogConfig(xgi::Output* out)
{
	*out<<"<div id = \"hard-config\" style=\"display: inline-block;\">\n"
		<<"<h4>Hardware Configuration</h4>\n"
		<<"<textarea id = \"tb_Hardware_Configuration\" rows=\"5\" cols=\"50\" height=\"10\" readonly = \"true\">\n"
		<< tb_Hardware_Configuration
		<<"</textarea>"
		<<"</div>\n";
}
void
pixel::tcds::PixelTCDSSupervisor::tabPresentation(xgi::Output* out)
{

	*out<<"<div class=\"container\">\n"
		<<"<h2>"<<appNamePlusInstance_<<"</h2>\n"
		<<"<ul class=\"nav nav-tabs\">\n"
		<<"<li class=\"active\"><a data-toggle=\"tab\" href=\"#home\">Configuration</a></li>\n"
		<<"<li><a data-toggle=\"tab\" href=\"#menu1\">Application Status</a></li>\n"
		<<"<li><a data-toggle=\"tab\" href=\"#menu2\">Expert Actions</a></li>\n"
		<<"</ul>\n\n"
		<<"<div class=\"tab-content\">\n"
		<<"<div id=\"home\" class=\"tab-pane fade in active\">\n"

		<<"<div >\n";
	tableConfig(out);
	tableRemoteInfo(out);
	*out<<"</div>\n";
	tableLogConfig(out);
	*out<<"<div>\n";

	*out<<"</div>\n"
		<<"</div>\n"

		<<"<div id=\"menu1\" class=\"tab-pane fade\">\n"
		<<"<h3>APPLICATION STATUS</h3>\n"
		<<"<p>Application Information.</p>\n";
	tableStatus(out);
	*out<<"<h3>HISTORY</h3>\n"
		<<"<p>Application status and command history.</p>\n";
	tableHistory(out);
	*out<<"</div>\n"
		<<"<div id=\"menu2\" class=\"tab-pane fade\">\n"
		<<"<h3>EXPERTS ACTIONS</h3>\n"
		<<"<p>Expert Actions.</p>\n";
	tableBgoString(out);
	*out<<"</div>\n</div>\n</div>\n\n";
}

void
pixel::tcds::PixelTCDSSupervisor::queryFSMState(xgi::Input* in, xgi::Output* out)
{
  queryFSMStateAction(true);
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::queryHwLeaseOwner(xgi::Input* in, xgi::Output* out)
{
  queryHwLeaseOwnerAction(true);
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::reset(xgi::Input* in, xgi::Output* out)
{
  resetAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::initialize(xgi::Input* in, xgi::Output* out)
{
  initializeAction();
  redirect(in, out);
}


void
pixel::tcds::PixelTCDSSupervisor::configure(xgi::Input* in, xgi::Output* out)
{
  configureAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::enable(xgi::Input* in, xgi::Output* out)
{
  enableAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::pause(xgi::Input* in, xgi::Output* out)
{
  pauseAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::resume(xgi::Input* in, xgi::Output* out)
{
  resumeAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::stop(xgi::Input* in, xgi::Output* out)
{
  stopAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::halt(xgi::Input* in, xgi::Output* out)
{
  haltAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::coldReset(xgi::Input* in, xgi::Output* out)
{
  coldResetAction();
  redirect(in, out);
}


void
pixel::tcds::PixelTCDSSupervisor::ttcResync(xgi::Input* in, xgi::Output* out)
{
  ttcResyncAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::ttcHardReset(xgi::Input* in, xgi::Output* out)
{
  ttcHardResetAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::renewHardwareLease(xgi::Input* in, xgi::Output* out)
{
  renewHardwareLeaseAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::readHardwareConfiguration(xgi::Input* in, xgi::Output* out)
{
  readHardwareConfigurationAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::sendL1A(xgi::Input* in, xgi::Output* out)
{
  sendL1AAction();
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::sendBgo(xgi::Input* in, xgi::Output* out)
{
  statusMsg_ = "";
  try
    {
      cgicc::Cgicc cgi(in);
      std::string myString= (cgi["CommandUInt"]->getValue());
      LOG4CPLUS_ERROR(logger_, "Using CommandUInt with value: -" + myString + "-");
      xdata::UnsignedInteger commandUInt = cgi["CommandUInt"]->getIntegerValue();
      sendBgoAction(commandUInt);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::sendBgoString(xgi::Input* in, xgi::Output* out)
{
  statusMsg_ = "";
  try
    {
      cgicc::Cgicc cgi(in);
      LOG4CPLUS_ERROR(logger_, "Using CommandString with value: -" + cgi["CommandString"]->getValue()+"-");
      xdata::String commandString = cgi["CommandString"]->getValue();
      sendBgoStringAction(commandString);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::sendBgoTrain(xgi::Input* in, xgi::Output* out)
{
  statusMsg_ = "";
  try
    {
      cgicc::Cgicc cgi(in);
      LOG4CPLUS_ERROR(logger_, "Using TrainString with value: -" + cgi["TrainString"]->getValue()+"-");
      xdata::String trainString = cgi["TrainString"]->getValue();
      sendBgoTrainAction(trainString);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::enableRandomTriggers(xgi::Input* in, xgi::Output* out)
{
  statusMsg_ = "";
  try
    {
      cgicc::Cgicc cgi(in);
      std::string myString= (cgi["FrequencyUInt"]->getValue());
      LOG4CPLUS_ERROR(logger_, "Using FrequencyUInt with value: -" + myString + "-");
      xdata::UnsignedInteger frequencyUInt = cgi["FrequencyUInt"]->getIntegerValue();
      enableRandomTriggersAction(frequencyUInt);
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::updateHardwareConfigurationFile(xgi::Input* in, xgi::Output* out)
{
  statusMsg_ = "";
  try
    {
      cgicc::Cgicc cgi(in);
      std::string fileNameString= (cgi["FileNameString"]->getValue());
      LOG4CPLUS_INFO(logger_, "Using FileNameString with value: -" + fileNameString + "-");
      // check first if file exists
      std::ifstream f(fileNameString.c_str());
      if (f.good()) {
        hwCfgFileName_ = fileNameString;
        readConfigFile();
      }
      else {
        LOG4CPLUS_ERROR(logger_, "File " + fileNameString + " does not exist. Skipping update.");
      }
      f.close();
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
  redirect(in, out);
}

void
pixel::tcds::PixelTCDSSupervisor::updateHardwareConfiguration(xgi::Input* in, xgi::Output* out)
{
  statusMsg_ = "";
  try
    {
      cgicc::Cgicc cgi(in);
      cgicc::const_file_iterator file = cgi.getFile("ConfigurationString");
      std::string configurationString = file->getData();
      if (file->getDataType() == "text/plain") {
        LOG4CPLUS_INFO(logger_, "Using the following ConfigurationString:\n" + configurationString );
        hwCfgString_ = configurationString;
      }
      else {
        LOG4CPLUS_ERROR(logger_, "File uploaded is not of type text/plain, but of type " + file->getDataType() + "! Skipping update." );
      }
    }
  catch (xcept::Exception& err)
    {
      std::string const msg = toolbox::toString("An error occured: '%s'.",
                                                err.what());
      LOG4CPLUS_ERROR(logger_, msg);
      statusMsg_ = msg;
      this->notifyQualified("error",err);
    }
  redirect(in, out);
}


void
pixel::tcds::PixelTCDSSupervisor::JSONAction()
{

}
void
pixel::tcds::PixelTCDSSupervisor::jsonUpdate(xgi::Input* const in, xgi::Output* const out)
{

    try
      {
        jsonUpdateCore(in, out);
      }
    catch (xcept::Exception& err)
      {
        std::string const msg =
          toolbox::toString("Failed to serve the JSON update. "
                            "Caught an exception: '%s'.", err.what());
        // ERROR(msg);
        // XCEPT_DECLARE(tcds::exception::RuntimeProblem, top, msg);
        // getOwnerApplication()->notifyQualified("error", top);
      }
}


std::string pixel::tcds::PixelTCDSSupervisor::formatTimestamp(toolbox::TimeVal const timestamp)
{
  return toolbox::TimeVal(timestamp).toString("%Y-%m-%d %H:%M:%S UTC",
                                              toolbox::TimeVal::gmt);
}

std::string pixel::tcds::PixelTCDSSupervisor::formatDeltaTString(toolbox::TimeVal const timeBegin, toolbox::TimeVal const timeEnd)
{
	std::stringstream result;
  toolbox::TimeVal deltaT = timeEnd - timeBegin;
  if (deltaT.sec() != 0)
    {
      result << deltaT.sec() << " second";
      if(deltaT.sec() > 1)
        {
          result << "s";
        }
    }
  if (deltaT.millisec() != 0)
    {
      if (result.str().size() != 0)
        {
          result << " and ";
        }
      result << deltaT.millisec() << " millisecond";
      if (deltaT.millisec() > 1)
        {
          result << "s";
        }
    }
  if (result.str().size() == 0)
    {
      result << "negligible time";
    }
  return result.str();

}
void
pixel::tcds::PixelTCDSSupervisor::jsonUpdateCore(xgi::Input* const in, xgi::Output* const out)
{
	toolbox::TimeInterval timeBegin(toolbox::TimeVal::gettimeofday());
	
  	tb_Config_state = fsm_.getStateName (fsm_.getCurrentState());
  	tb_Config_TCDS = "class '" + tcdsAppClassName() + "', instance number " + std::string(itoa(tcdsAppInstance()));
  	tb_Config_sessionID = "'" + sessionId() + "'";
  	tb_Config_renewInteval = hwLeaseRenewalInterval();
  	tb_Config_runNumber = runNumber_.toString();
  	tb_Config_hardware = hwCfgFileName_.toString();
	tb_Hardware_Configuration = hwCfgString_.toString();
	std::replace( tb_Hardware_Configuration.begin(), tb_Hardware_Configuration.end(), '"', '@');
	std::replace( tb_Hardware_Configuration.begin(), tb_Hardware_Configuration.end(), '\n', '<');
	

	if (statusMsg_.toString().find("error") != std::string::npos)
    tb_Config_statusMsg = "<font color='red'>" + statusMsg_.toString() + "</font>";
  else
    tb_Config_statusMsg =  "'"+ statusMsg_.toString() + "'";
	

  	std::string const hwLeaseOwnerId(tcdsHwLeaseOwnerId_.toString());
  	std::string tmpStr("");
    if (hwLeaseOwnerId == "")
    {
        tmpStr = " (hardware not leased/lease expired)";
    }
	queryFSMStateAction();
	queryHwLeaseOwnerAction();
	
  	tb_Remote_tcdsState = tcdsState_.toString();
  	tb_Remote_Hardware = "'" + hwLeaseOwnerId + "'" + tmpStr;

  	toolbox::TimeVal timeNow(toolbox::TimeVal::gettimeofday());
	toolbox::TimeInterval upTime = timeNow - timeStart_;
	toolbox::TimeInterval upTime_now = timeNow;

  	tb_Status_uptime = upTime.toString();
    tb_Status_timenow = formatTimestamp(timeNow);
	//std::cout<<"timeNow="<<timeNow<<std::endl;
	
	toolbox::TimeInterval timeEnd(toolbox::TimeVal::gettimeofday());
	
	tb_Status_latestMonitoringDuration = formatDeltaTString(timeBegin, timeEnd);
  	
  // Check if the other side supports gzip.
  bool doGZIP = false;
  std::string const acceptEncoding = in->getenv("ACCEPT_ENCODING");
  if (!acceptEncoding.empty() && (acceptEncoding.find("gzip") != std::string::npos))
    {
      doGZIP = true;
    }

  // Prepare the actual JSON contents.
  std::stringstream tmp("");
  std::string jsonTmp = "\"tb_Config_state\" : \"" + tb_Config_state + "\"";
  jsonTmp += ",\n\"tb_Status_timenow\" : \"" + tb_Status_timenow + "\"";
  jsonTmp += ",\n\"tb_Status_latestMonitoringDuration\" : \"" + tb_Status_latestMonitoringDuration + "\"";
  jsonTmp += ",\n\"tb_Status_uptime\" : \"" + tb_Status_uptime + "\"";
  jsonTmp += ",\n\"tb_Config_TCDS\" : \"" + tb_Config_TCDS + "\"";
  jsonTmp += ",\n\"tb_Config_sessionID\" : \"" + tb_Config_sessionID + "\"";
  jsonTmp += ",\n\"tb_Config_renewInteval\" : \"" + tb_Config_renewInteval + "\"";
  jsonTmp += ",\n\"tb_Config_runNumber\" : \"" + tb_Config_runNumber + "\"";
  jsonTmp += ",\n\"tb_Config_hardware\" : \"" + tb_Config_hardware + "\"";
  jsonTmp += ",\n\"tb_Config_statusMsg\" : \"" + tb_Config_statusMsg + "\"";
  jsonTmp += ",\n\"tb_Remote_tcdsState\" : \"" + tb_Remote_tcdsState + "\"";
  jsonTmp += ",\n\"tb_Remote_Hardware\" : \"" + tb_Remote_Hardware + "\"";
  jsonTmp += ",\n\"tb_Hardware_Configuration\" : \"" + tb_Hardware_Configuration + "\"";
  
  
  if (!jsonTmp.empty())
    {
      if (!tmp.str().empty())
        {
          tmp << ",\n";
        }
      tmp << jsonTmp;
    }


  std::string jsonContents = "{\n" + tmp.str() + "\n}";


  // Stuff everything into the output.
  out->getHTTPResponseHeader().addHeader("Content-Type", "application/json");
  // if (doGZIP)
  //   {
  //     out->getHTTPResponseHeader().addHeader("Content-Encoding", "gzip");
  //     std::string const tmp = tcds::utils::compressString(jsonContents);
  //     *out << tmp;
  //   }
  // else
  //   {
      *out << jsonContents;
    // }
	
	
}

void
pixel::tcds::PixelTCDSSupervisor::onException(xcept::Exception& err)
{
  std::string const msg =
    toolbox::toString("An error occurred in the hardware lease renewal thread : '%s'.",
                      err.what());
  statusMsg_ = msg;
  LOG4CPLUS_ERROR(logger_, "PixeliCISupervisor");
  LOG4CPLUS_ERROR(logger_, msg.c_str());
}

/**
 xoap bindings
**/

xoap::MessageReference pixel::tcds::PixelTCDSSupervisor::fireEvent ( xoap::MessageReference msg ) throw ( xoap::exception::Exception )
{
  xoap::SOAPPart part = msg->getSOAPPart();
  xoap::SOAPEnvelope env = part.getEnvelope();
  xoap::SOAPBody body = env.getBody();
  DOMNode* node = body.getDOMNode();
  DOMNodeList* bodyList = node->getChildNodes();

  for ( unsigned int i = 0; i < bodyList->getLength(); i++ )
  {
    std::string responseString = "Response";
    DOMNode* command = bodyList->item(i);

    if ( command->getNodeType() == DOMNode::ELEMENT_NODE )
    {
      std::string commandName = xoap::XMLCh2String(command->getLocalName());
      // actual work
      if (commandName == "Handshake")
      {
        try
        {
          Attribute_Vector parametersReceived(1);
          parametersReceived[0].name_="TCDSSessionID";
          Receive(msg, parametersReceived);
          sessionId_=parametersReceived[0].value_;
	  LOG4CPLUS_INFO(getApplicationLogger(), "Received TCDSSessionId:" << std::string(sessionId_));
        }
        catch (xcept::Exception& err)
        {
          responseString = toolbox::toString("An error occured: '%s'.",
                                             err.what());
          LOG4CPLUS_ERROR(logger_, responseString);
          this->notifyQualified("error",err);
        }
      }

      else if (commandName == "ReceiveConfigString")
        {
          try
            {
              Attribute_Vector parametersReceived(1);
              parametersReceived[0].name_="TCDSConfigString";
              Receive(msg, parametersReceived);
              hwCfgString_=parametersReceived[0].value_;
              hwCfgFileName_="received from PixelSupervisor";
              receivedCfgString_=true;
            }
          catch (xcept::Exception& err)
            {
              responseString = toolbox::toString("An error occured: '%s'.",
                                                 err.what());
              LOG4CPLUS_ERROR(logger_, responseString);
              this->notifyQualified("error",err);
            }
        }

      else if (commandName == "Reset")
      {
        resetAction();
      }
      else if (commandName == "Initialize")
      {
        initializeAction();
      }
      else if (commandName == "Configure")
      {
        configureAction();
      }
      else if (commandName == "Start")
      {
        try{
          Attribute_Vector parametersReceived(1);
          parametersReceived[0].name_="RunNumber";
          Receive(msg, parametersReceived);
          runNumber_=parametersReceived[0].value_;
        }
        catch (xcept::Exception& err){
          responseString = toolbox::toString("An error occured: '%s'.",
                                             err.what());
          LOG4CPLUS_ERROR(logger_, responseString);
          this->notifyQualified("error",err);
        }
        enableAction();
      }
      else if ((commandName == "Pause") || (commandName == "Suspend"))
      {
        pauseAction();
      }
      else if (commandName == "Resume")
      {
        resumeAction();
      }
      else if (commandName == "Stop")
      {
        stopAction();
      }
      else if (commandName == "Halt")
      {
        haltAction();
      }
      else if (commandName == "ColdReset")
      {
        coldResetAction();
      }
      else if (commandName == "TTCResync")
      {
        ttcResyncAction();
      }
      else if (commandName == "TTCHardReset")
      {
        ttcHardResetAction();
      }
      else if (commandName == "RenewHardwareLease")
      {
        renewHardwareLeaseAction();
      }
      else if (commandName == "ReadHardwareConfiguration")
      {
        readHardwareConfigurationAction();
      }
      else if (commandName == "SendL1A")
      {
        sendL1AAction();
      }
	  else if (commandName == "update")
      {
        JSONAction();
      }
      else if (commandName == "SendBgo")
      {
        // find out whether BgoNumber or BgoName or BgoTrain have been passed
        LOG4CPLUS_ERROR(logger_, "SendBgo not yet implemented");
        // missing
      }
      else if ((commandName == "QueryFSMState") || (commandName == "FSMStateRequest"))
      {
        queryFSMStateAction();
        std::string state = fsm_.getStateName (fsm_.getCurrentState());
        return MakeSOAPMessageReference(state);
      }

      xoap::MessageReference reply = xoap::createMessage();
      xoap::SOAPEnvelope envelope = reply->getSOAPPart().getEnvelope();
      xoap::SOAPName responseName = envelope.createName(commandName + responseString, "xdaq", XDAQ_NS_URI);

      xoap::SOAPBodyElement responseElement = envelope.getBody().addBodyElement(responseName);
      xoap::SOAPName stateName = envelope.createName("response", "xdaq", XDAQ_NS_URI);
      xoap::SOAPElement stateElement = responseElement.addChildElement(stateName);

      LOG4CPLUS_INFO(logger_, "Sending response to received SOAP command - command was: " + commandName);
      return reply;
    }
  }

  XCEPT_RAISE(xcept::Exception, "SOAP command not found");
}

/**
 further functions
**/

void
pixel::tcds::PixelTCDSSupervisor::readConfigFile()
{

  // open hwCfgFileName_ and store to hwCfgString_
  std::string responseString;
  std::ifstream hwCfgFile;
  hwCfgFile.open(hwCfgFileName_.toString().c_str());
  if (hwCfgFile.good()) {
    responseString = "Reading hwCfgFile for storing: " + hwCfgFileName_.toString();
    LOG4CPLUS_INFO(logger_, responseString);
    std::string inString((std::istreambuf_iterator<char>(hwCfgFile)),
                     std::istreambuf_iterator<char>());
    LOG4CPLUS_INFO(logger_, inString);
    hwCfgString_ = inString;

  }
  else {
    responseString = "Error opening file " + hwCfgFileName_.toString();
    LOG4CPLUS_ERROR(logger_, responseString);
  }
  hwCfgFile.close();

}
