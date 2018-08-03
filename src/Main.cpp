#include <iostream>
#include <canbus.hh>
#include <memory>
#include <motors_elmo_ds402/Controller.hpp>
#include <iodrivers_base/Driver.hpp>
#include <string>
#include <iomanip>
#include <signal.h>
#include <cstring>

using namespace std;
using namespace motors_elmo_ds402;

int usage()
{
    cout << "motors_elmo_ds402_ctl CAN_DEVICE CAN_DEVICE_TYPE CAN_ID COMMAND\n";
    cout << "  reset     # resets the drive";
    cout << "  get-state # displays the drive's internal state\n";
    cout << "  set-state NEW_STATE # changes the drive's internal state\n";
    cout << "    SHUTDOWN, SWITCH_ON, ENABLE_OPERATION, DISABLE_VOLTAGE, QUICK_STOP,\n";
    cout << "    DISABLE_OPERATION, FAULT_RESET\n";
    cout << endl;
    return 1;
}

template<typename T>
struct STRINGS {
    std::string name;
    T value;
};

STRINGS<StatusWord::State> StateStrings[] = {
    { "NOT_READY_TO_SWITCH_ON", StatusWord::NOT_READY_TO_SWITCH_ON },
    { "SWITCH_ON_DISABLED", StatusWord::SWITCH_ON_DISABLED },
    { "READY_TO_SWITCH_ON", StatusWord::READY_TO_SWITCH_ON },
    { "SWITCH_ON", StatusWord::SWITCH_ON },
    { "OPERATION_ENABLED", StatusWord::OPERATION_ENABLED },
    { "QUICK_STOP_ACTIVE", StatusWord::QUICK_STOP_ACTIVE },
    { "FAULT_REACTION_ACTIVE", StatusWord::FAULT_REACTION_ACTIVE },
    { "FAULT", StatusWord::FAULT },
    { "", StatusWord::FAULT }
};

STRINGS<ControlWord::Transition> TransitionStrings[] = {
    { "SHUTDOWN", ControlWord::SHUTDOWN },
    { "SWITCH_ON", ControlWord::SWITCH_ON },
    { "ENABLE_OPERATION", ControlWord::ENABLE_OPERATION },
    { "DISABLE_VOLTAGE", ControlWord::DISABLE_VOLTAGE },
    { "QUICK_STOP", ControlWord::QUICK_STOP },
    { "DISABLE_OPERATION", ControlWord::DISABLE_OPERATION },
    { "FAULT_RESET", ControlWord::FAULT_RESET },
    { "", ControlWord::FAULT_RESET }
};

STRINGS<OPERATION_MODES> OperationModeStrings[] = {
    { "NONE", OPERATION_MODE_NONE },
    { "PROFILED_POSITION", OPERATION_MODE_PROFILED_POSITION },
    { "VELOCITY", OPERATION_MODE_VELOCITY },
    { "PROFILED_VELOCITY", OPERATION_MODE_PROFILED_VELOCITY },
    { "PROFILED_TORQUE", OPERATION_MODE_PROFILED_TORQUE },
    { "HOMING", OPERATION_MODE_HOMING },
    { "CYCLIC_SYNCHRONOUS_POSITION", OPERATION_MODE_CYCLIC_SYNCHRONOUS_POSITION },
    { "CYCLIC_SYNCHRONOUS_VELOCITY", OPERATION_MODE_CYCLIC_SYNCHRONOUS_VELOCITY },
    { "CYCLIC_SYNCHRONOUS_TORQUE", OPERATION_MODE_CYCLIC_SYNCHRONOUS_TORQUE },
    { "", OPERATION_MODE_PROFILED_POSITION }
};

template<typename T>
std::string toString(T const* conversions, decltype(T::value) value)
{
    for (T const* conv = conversions; !conv->name.empty(); ++conv) {
        if (conv->value == value) {
            return conv->name;
        }
    }
    throw std::invalid_argument("unknown value");
}

template<typename T>
decltype(T::value) fromString(T const* conversions, std::string const& name)
{
    for (T const* conv = conversions; !conv->name.empty(); ++conv) {
        if (conv->name == name) {
            return conv->value;
        }
    }
    throw std::invalid_argument("unknown string");
}

struct DisplayStats
{
    iodrivers_base::Driver* driver;

    DisplayStats(iodrivers_base::Driver* driver)
        : driver(driver) {}

    ~DisplayStats() {
        if (driver) {
            auto status = driver->getStatus();
            std::cerr << "tx=" << status.tx << " good_rx=" << status.good_rx << " bad_rx=" << status.bad_rx << std::endl;
        }
    }
};

static void writeObject(canbus::Driver& device, canbus::Message const& query,
    motors_elmo_ds402::Controller& controller,
    base::Time timeout = base::Time::fromMilliseconds(100))
{

    device.write(query);
    device.setReadTimeout(timeout.toMilliseconds());
    while(true)
    {
        canbus::Message msg = device.read();
        if (controller.process(msg).isAck()) {
            return;
        }
    }
}

static void writeObjects(canbus::Driver& device, vector<canbus::Message> const& query,
    motors_elmo_ds402::Controller& controller,
    base::Time timeout = base::Time::fromMilliseconds(100))
{
    for(auto const& msg : query) {
        writeObject(device, msg, controller, timeout);
    }
}

static void queryObject(canbus::Driver& device, canbus::Message const& query,
    motors_elmo_ds402::Controller& controller,
    uint64_t updateId,
    base::Time timeout = base::Time::fromMilliseconds(1000))
{
    device.write(query);
    device.setReadTimeout(timeout.toMilliseconds());
    while(true)
    {
        canbus::Message msg = device.read();
        if (controller.process(msg).hasOneUpdated(updateId)) {
            return;
        }
    }
}

static void queryObjects(canbus::Driver& device, std::vector<canbus::Message> const& query,
    motors_elmo_ds402::Controller& controller,
    uint64_t updateId,
    base::Time timeout = base::Time::fromMilliseconds(100))
{
    for(auto const& msg : query) {
        queryObject(device, msg, controller, updateId, timeout);
    }
}

struct Deinit
{
    canbus::Driver& mCan;
    Controller& mController;

    Deinit(canbus::Driver& d, Controller& c)
        : mCan(d), mController(c) {}
    ~Deinit()
    {
        writeObject(mCan,
            mController.send(ControlWord(ControlWord::SHUTDOWN, true)),
            mController);
    }
};

bool interrupted = false;
void sigint(int)
{
    interrupted = true;
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        return usage();
    }

    std::string can_device(argv[1]);
    std::string can_device_type(argv[2]);
    int8_t node_id(stoi(argv[3]));
    std::string cmd(argv[4]);

    unique_ptr<canbus::Driver> device;
    try {
        device.reset(canbus::openCanDevice(can_device, can_device_type));
    }
    catch(std::exception const& e) {
        std::cerr << "Failed to open the CAN device: "
            << e.what() << std::endl;
        return 1;
    }

    DisplayStats stats(dynamic_cast<iodrivers_base::Driver*>(device.get()));
    Controller controller(node_id);

    struct sigaction sigint_handler;
    std::memset(&sigint_handler, 0, sizeof(sigint_handler));
    sigint_handler.sa_handler = &sigint;
    sigemptyset(&sigint_handler.sa_mask);
    if (-1 == sigaction(SIGINT, &sigint_handler, 0))
    {
        std::cerr << "failed to install SIGINT handler" << std::endl;
        return 1;
    }
    sigset_t unblock_sigint;
    sigemptyset(&unblock_sigint);
    sigaddset(&unblock_sigint, SIGINT);
    if (-1 == sigprocmask(SIG_UNBLOCK, &unblock_sigint, NULL))
    {
        std::cerr << "failed to install SIGINT handler" << std::endl;
        return 1;
    }

    if (cmd == "reset")
    {
        if (argc != 5)
            return usage();

        queryObject(*device,
            controller.queryNodeStateTransition(canopen_master::NODE_RESET),
            controller, UPDATE_HEARTBEAT, base::Time::fromMilliseconds(5000));
        controller.getNodeState();
    }
    else if (cmd == "get-state")
    {
        if (argc != 5)
            return usage();

        queryObject(*device, controller.queryStatusWord(), controller,
            UPDATE_STATUS_WORD);
        StatusWord status = controller.getStatusWord();
        cout << toString(StateStrings, status.state) << "\n"
            << "  voltageEnabled      " << status.voltageEnabled << "\n"
            << "  warning             " << status.warning << "\n"
            << "  targetReached       " << status.targetReached << "\n"
            << "  internalLimitActive " << status.internalLimitActive << std::endl;

        queryObject(*device, controller.queryOperationMode(),
            controller, UPDATE_OPERATION_MODE);
        auto mode = controller.getOperationMode();
        cout << "Operation Mode: " << toString(OperationModeStrings, mode) << "\n";

        queryObjects(*device, controller.queryFactors(),
            controller, UPDATE_FACTORS);
        queryObjects(*device, controller.queryJointState(),
            controller, UPDATE_JOINT_STATE);
        auto jointState = controller.getJointState();
        cout << "Current joint state:\n" <<
            "  position " << jointState.position << "\n" <<
            "  speed    " << jointState.speed << "\n" <<
            "  effort   " << jointState.effort << "\n" <<
            "  current  " << jointState.raw << endl;
    }
    else if (cmd == "get-config")
    {
        queryObjects(*device, controller.queryFactors(),
            controller, UPDATE_FACTORS);
        Factors factors = controller.getFactors();
        cout << "Scale factors:\n"
            << "  encoder " << factors.encoderTicks <<
                " / " << factors.encoderRevolutions << "\n"
            << "  gearRatio    " << factors.gearMotorShaftRevolutions <<
                " / " << factors.gearDrivingShaftRevolutions << "\n"
            << "  feedConstant " << factors.feedLength <<
                " / " << factors.feedDrivingShaftRevolutions << "\n"
            << "  ratedTorque  " << factors.ratedTorque << "\n"
            << "  ratedCurrent " << factors.ratedCurrent << endl;

        queryObjects(*device, controller.queryJointLimits(),
            controller, UPDATE_JOINT_LIMITS);
        auto jointLimits = controller.getJointLimits();
        cout << "Current joint limits:\n" <<
            "  position     [" << jointLimits.min.position << ", " << jointLimits.max.position << "]\n" <<
            "  speed        [" << jointLimits.min.speed << ", " << jointLimits.max.speed << "]\n" <<
            "  acceleration [" << jointLimits.min.acceleration << ", " << jointLimits.max.acceleration << "]\n" <<
            "  effort       [" << jointLimits.min.effort << ", " << jointLimits.max.effort << "]\n" <<
            "  current      [" << jointLimits.min.raw << ", " << jointLimits.max.raw << "]" << endl;
    }
    else if (cmd == "set-state")
    {
        if (argc != 6)
            return usage();

        auto transition = fromString(TransitionStrings, argv[5]);
        writeObject(*device, controller.send(ControlWord(transition, true)), controller);
        queryObject(*device, controller.queryStatusWord(), controller,
            UPDATE_STATUS_WORD);
        StatusWord status = controller.getStatusWord();
        cout << "New state: " << toString(StateStrings, status.state) << endl;
    }
    else if (cmd == "stop")
    {
        writeObject(*device,
            controller.setOperationMode(OPERATION_MODE_NONE),
            controller);
    }
    else if (cmd == "set-torque")
    {
        if (argc != 6)
            return usage();

        queryObjects(*device, controller.queryFactors(),
            controller, UPDATE_FACTORS);

        device->write(controller.queryNodeStateTransition(
            canopen_master::NODE_ENTER_PRE_OPERATIONAL));
        writeObjects(*device, controller.queryPeriodicJointStateUpdate(0, 1),
            controller);
        device->write(controller.queryNodeStateTransition(
            canopen_master::NODE_START));

        double target_torque = atof(argv[5]);
        writeObject(*device,
            controller.setOperationMode(OPERATION_MODE_CYCLIC_SYNCHRONOUS_TORQUE),
            controller);
        writeObject(*device,
            controller.send(ControlWord(ControlWord::SHUTDOWN, true)),
            controller);
        writeObject(*device,
            controller.send(ControlWord(ControlWord::SWITCH_ON, true)),
            controller);
        writeObject(*device,
            controller.send(ControlWord(ControlWord::ENABLE_OPERATION, true)),
            controller);
        canbus::Message sync = controller.querySync();
        device->write(sync);
        controller.setEncoderScaleFactor(1);
        writeObject(*device,
            controller.setTorqueTarget(0),
            controller);
        while(!interrupted)
        {
            writeObject(*device,
                controller.setTorqueTarget(target_torque),
                controller);
            usleep(10000);
            device->write(sync);

            std::cout << base::Time::now().toMilliseconds() << " ";

            Update state = Update();
            while (!interrupted && !state.isUpdated(UPDATE_JOINT_STATE))
            {
                canbus::Message msg = device->read();
                state.merge(controller.process(msg));
            }

            if (!interrupted) {
                base::JointState jointState = controller.getJointState();
                if (controller.getZeroPosition() == 0)
                    controller.setZeroPosition(controller.getRawPosition());
                cout << setw(10) << jointState.position << " "
                    << setw(10) << jointState.speed << " "
                    << setw(10) << jointState.effort << " "
                    << setw(10) << jointState.raw << endl;
            }
        }
        writeObject(*device,
            controller.setOperationMode(OPERATION_MODE_NONE),
            controller);
    }
    else if (cmd == "save")
    {
        if (argc != 5)
            return usage();
        writeObject(*device, controller.querySave(), controller);
    }
    else if (cmd == "load")
    {
        if (argc != 5)
            return usage();
        writeObject(*device, controller.queryLoad(), controller);
    }
    else if (cmd == "monitor-joint-state")
    {
        queryObjects(*device, controller.queryFactors(),
            controller, UPDATE_FACTORS);
        bool use_sync = true;
        vector<canbus::Message> pdoSetup;
        if (argc == 7) {
            if (string(argv[5]) == "--time") {
                use_sync = false;
                pdoSetup = controller.queryPeriodicJointStateUpdate(
                    0, base::Time::fromMilliseconds(atoi(argv[6])));
                }
            else {
                std::cerr << "Invalid argument to 'monitor-joint-state'" << std::endl;
                return usage();
            }
        }
        else if (argc != 5) {
            return usage();
        }
        else {
            pdoSetup = controller.queryPeriodicJointStateUpdate(0, 1);
        }
        device->write(controller.queryNodeStateTransition(
            canopen_master::NODE_ENTER_PRE_OPERATIONAL));
        writeObjects(*device, pdoSetup, controller);
        device->write(controller.queryNodeStateTransition(
            canopen_master::NODE_START));
        device->setReadTimeout(1500);

        canbus::Message sync = controller.querySync();
        if (use_sync)
            device->write(sync);

        cout << setw(10) << "Position" << " "
            << setw(10) << "Speed" << " "
            << setw(10) << "Effort" << " "
            << setw(10) << "Current" << endl;

        Update state;
        while(true)
        {
            state = Update();
            if (use_sync)
                device->write(sync);

            while (!interrupted && !state.isUpdated(UPDATE_JOINT_STATE))
            {
                canbus::Message msg = device->read();
                state.merge(controller.process(msg));
            }

            if (interrupted)
                break;

            base::JointState jointState = controller.getJointState();
            cout << setw(10) << jointState.position << " "
                << setw(10) << jointState.speed << " "
                << setw(10) << jointState.effort << " "
                << setw(10) << jointState.raw << endl;
        }
    }
    return 0;
}
