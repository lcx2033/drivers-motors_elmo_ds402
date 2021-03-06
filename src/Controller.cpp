#include <motors_elmo_ds402/Controller.hpp>

using namespace std;
using namespace motors_elmo_ds402;

Controller::Controller(uint8_t nodeId)
    : mCanOpen(nodeId)
    , mRatedTorque(base::unknown<double>())
{
    setRaw<PositionEncoderResolutionNum>(1);
    setRaw<PositionEncoderResolutionDen>(1);
    setRaw<GearRatioNum>(1);
    setRaw<GearRatioDen>(1);
    setRaw<FeedConstantNum>(1);
    setRaw<FeedConstantDen>(1);
    setRaw<MotorRatedCurrent>(1);
    setRaw<MotorRatedTorque>(1);
}

void Controller::setRatedTorque(double ratedTorque)
{
    mRatedTorque = ratedTorque;
}

double Controller::getRatedTorque() const
{
    return mRatedTorque;
}

canbus::Message Controller::querySync() const
{
    return mCanOpen.sync();
}

canbus::Message Controller::queryNodeState() const
{
    return mCanOpen.queryState();
}

canbus::Message Controller::queryCANControllerStatus() const
{
    return queryObject<CANControllerStatus>();
}

CANControllerStatus Controller::getCANControllerStatus() const
{
    return get<CANControllerStatus>();
}

canbus::Message Controller::queryNodeStateTransition(
            canopen_master::NODE_STATE_TRANSITION transition) const
{
    return mCanOpen.queryStateTransition(transition);
}

canopen_master::NODE_STATE Controller::getNodeState() const
{
    return mCanOpen.getState();
}

canbus::Message Controller::queryStatusWord() const
{
    return queryObject<StatusWord>();
}

canbus::Message Controller::queryOperationMode() const
{
    return queryObject<ModesOfOperation>();
}

OPERATION_MODES Controller::getOperationMode() const
{
    return static_cast<OPERATION_MODES>(getRaw<ModesOfOperation>());
}

canbus::Message Controller::setOperationMode(OPERATION_MODES mode) const
{
    return mCanOpen.download(
        ModesOfOperation::OBJECT_ID,
        ModesOfOperation::OBJECT_SUB_ID,
        static_cast<int8_t>(mode));
}

std::vector<canbus::Message> Controller::queryFactors()
{
    return std::vector<canbus::Message>
    {
        queryObject<PositionEncoderResolutionNum>(),
        queryObject<PositionEncoderResolutionDen>(),
        queryObject<GearRatioNum>(),
        queryObject<GearRatioDen>(),
        queryObject<FeedConstantNum>(),
        queryObject<FeedConstantDen>(),
        queryObject<VelocityFactorNum>(),
        queryObject<VelocityFactorDen>(),
        queryObject<MotorRatedCurrent>(),
        queryObject<MotorRatedTorque>()
    };
}

canbus::Message Controller::setTorqueTarget(double target)
{
    if (base::isUnknown(mFactors.ratedTorque))
        throw std::logic_error("must query or set rated torque before using setTorqueTarget");

    double canopen_target = (target / mFactors.ratedTorque) * 1000;
    if (canopen_target < -32767 || canopen_target > 32768)
        throw std::out_of_range("torque value out of range");

    return mCanOpen.download(TargetTorque::OBJECT_ID, TargetTorque::OBJECT_SUB_ID,
        static_cast<int16_t>(canopen_target));
}

void Controller::setMotorParameters(MotorParameters const& parameters)
{
    mMotorParameters = parameters;

    if (parameters.encoderTicks)
        setRaw<PositionEncoderResolutionNum>(parameters.encoderTicks);
    if (parameters.encoderRevolutions)
        setRaw<PositionEncoderResolutionDen>(parameters.encoderRevolutions);
    if (parameters.gearMotorShaftRevolutions)
        setRaw<GearRatioNum>(parameters.gearMotorShaftRevolutions);
    if (parameters.gearDrivingShaftRevolutions)
        setRaw<GearRatioDen>(parameters.gearDrivingShaftRevolutions);
    if (parameters.feedLength)
        setRaw<FeedConstantNum>(parameters.feedLength);
    if (parameters.feedDrivingShaftRevolutions)
        setRaw<FeedConstantDen>(parameters.feedDrivingShaftRevolutions);
    if (!base::isUnknown(parameters.torqueConstant)) {
        uint64_t current_mA = getRaw<MotorRatedCurrent>();
        setRaw<MotorRatedTorque>(current_mA * parameters.torqueConstant);
    }

    try {
        mFactors = computeFactors();
    }
    catch(canopen_master::ObjectNotRead) {}
}

Factors Controller::getFactors() const
{
    return mFactors;
}

Factors Controller::computeFactors() const
{
    Factors factors;
    factors.encoderTicks = getRaw<PositionEncoderResolutionNum>();
    factors.encoderRevolutions = getRaw<PositionEncoderResolutionDen>();
    factors.gearMotorShaftRevolutions = getRaw<GearRatioNum>();
    factors.gearDrivingShaftRevolutions = getRaw<GearRatioDen>();
    factors.feedLength = getRaw<FeedConstantNum>();
    factors.feedDrivingShaftRevolutions = getRaw<FeedConstantDen>();
    factors.ratedTorque  = static_cast<double>(getRaw<MotorRatedTorque>()) / 1000;
    factors.ratedCurrent = static_cast<double>(getRaw<MotorRatedCurrent>()) / 1000;
    factors.update();
    return factors;
}

#define MODE_UPDATE_CASE(mode, object) \
    case canopen_master::StateMachine::mode: \
        update |= object::UPDATE_ID; \
        break;

#define SDO_UPDATE_CASE(object) \
    case (static_cast<uint32_t>(object::OBJECT_ID) << 8 | object::OBJECT_SUB_ID): \
        update |= object::UPDATE_ID; \
        break;

Update Controller::process(canbus::Message const& msg)
{
    uint64_t update = 0;
    auto canUpdate = mCanOpen.process(msg);
    switch(canUpdate.mode)
    {
        MODE_UPDATE_CASE(PROCESSED_HEARTBEAT, Heartbeat);
        case canopen_master::StateMachine::PROCESSED_SDO_INITIATE_DOWNLOAD:
        {
            // Ack of a upload request
            auto object = canUpdate.updated[0];
            return Update::Ack(object.first, object.second);
        }

        default: ; // we just ignore the rest, we really don't care
    };

    for (auto it = canUpdate.begin(); it != canUpdate.end(); ++it)
    {
        uint32_t fullId = static_cast<uint32_t>(it->first) << 8 | it->second;

        switch(fullId)
        {
            SDO_UPDATE_CASE(StatusWord);
            SDO_UPDATE_CASE(ModesOfOperation);

            // UPDATE_FACTORS
            SDO_UPDATE_CASE(PositionEncoderResolutionNum);
            SDO_UPDATE_CASE(PositionEncoderResolutionDen);
            SDO_UPDATE_CASE(VelocityEncoderResolutionNum);
            SDO_UPDATE_CASE(VelocityEncoderResolutionDen);
            SDO_UPDATE_CASE(AccelerationFactorNum);
            SDO_UPDATE_CASE(AccelerationFactorDen);
            SDO_UPDATE_CASE(GearRatioNum);
            SDO_UPDATE_CASE(GearRatioDen);
            SDO_UPDATE_CASE(FeedConstantNum);
            SDO_UPDATE_CASE(FeedConstantDen);
            SDO_UPDATE_CASE(VelocityFactorNum);
            SDO_UPDATE_CASE(VelocityFactorDen);
            SDO_UPDATE_CASE(MotorRatedCurrent);
            SDO_UPDATE_CASE(MotorRatedTorque);

            // UPDATE_JOINT_STATE
            SDO_UPDATE_CASE(PositionActualInternalValue);
            SDO_UPDATE_CASE(VelocityActualValue);
            SDO_UPDATE_CASE(CurrentActualValue);

            // UPDATE_JOINT_LIMITS
            SDO_UPDATE_CASE(SoftwarePositionLimitMin);
            SDO_UPDATE_CASE(SoftwarePositionLimitMax);
            SDO_UPDATE_CASE(MaxMotorSpeed);
            SDO_UPDATE_CASE(MaxAcceleration);
            SDO_UPDATE_CASE(MaxDeceleration);
            SDO_UPDATE_CASE(MaxCurrent);
        }
    }

    if (update & UPDATE_FACTORS) {
        // If the user explicitely wrote motor parameters, we apply them again
        // The method re-computed the factors. There's no need to do it
        // explicitely
        setMotorParameters(mMotorParameters);
    }

    return Update::UpdatedObjects(update);
}

StatusWord Controller::getStatusWord() const
{
    return get<StatusWord>();
}

template<typename T>
void Controller::setRaw(typename T::OBJECT_TYPE value)
{
    return mCanOpen.set<typename T::OBJECT_TYPE>(T::OBJECT_ID, T::OBJECT_SUB_ID, value);
}

template<typename T>
typename T::OBJECT_TYPE Controller::getRaw() const
{
    return mCanOpen.get<typename T::OBJECT_TYPE>(T::OBJECT_ID, T::OBJECT_SUB_ID);
}

template<typename T>
T Controller::get() const
{
    return parse<T, typename T::OBJECT_TYPE>(getRaw<T>());
}

template<typename T>
canbus::Message Controller::queryObject() const
{
    return mCanOpen.upload(T::OBJECT_ID, T::OBJECT_SUB_ID);
}

std::vector<canbus::Message> Controller::queryJointState() const
{
    // NOTE: we don't need to query TorqueActualValue. Given how bot this and
    // CurrentActualValue are encoded, they contain the same value
    return vector<canbus::Message> {
        queryObject<PositionActualInternalValue>(),
        queryObject<VelocityActualValue>(),
        queryObject<CurrentActualValue>()
    };
}

int64_t Controller::getZeroPosition() const
{
    return mZeroPosition;
}

void Controller::setZeroPosition(int64_t base)
{
    mZeroPosition = base;
}

int64_t Controller::getRawPosition() const
{
    return getRaw<PositionActualInternalValue>();
}

void Controller::setEncoderScaleFactor(double scale)
{
    mFactors.encoderScaleFactor = scale;
    mFactors.update();
}

base::JointState Controller::getJointState(uint64_t fields) const
{
    base::JointState state;
    if (fields & UPDATE_JOINT_POSITION)
    {
        auto position = getRaw<PositionActualInternalValue>() - mZeroPosition;
        state.position = mFactors.rawToEncoder(position);
    }
    if (fields & UPDATE_JOINT_VELOCITY)
    {
        auto velocity = getRaw<VelocityActualValue>();
        state.speed    = mFactors.rawToEncoder(velocity);
    }
    if (fields & UPDATE_JOINT_CURRENT) {
        // See comment in queryJointState
        auto current_and_torque = getRaw<CurrentActualValue>();
        state.raw      = mFactors.rawToCurrent(current_and_torque);
        state.effort   = mFactors.rawToTorque(current_and_torque);
    }
    return state;
}

vector<canbus::Message> Controller::queryJointLimits() const
{
    return vector<canbus::Message> {
        queryObject<SoftwarePositionLimitMin>(),
        queryObject<SoftwarePositionLimitMax>(),
        queryObject<MaxMotorSpeed>(),
        queryObject<MaxAcceleration>(),
        queryObject<MaxDeceleration>(),
        queryObject<MaxCurrent>()
    };
}

base::JointLimitRange Controller::getJointLimits() const
{
    base::JointState min;
    base::JointState max;

    int32_t rawPositionMin = getRaw<SoftwarePositionLimitMin>();
    int32_t rawPositionMax = getRaw<SoftwarePositionLimitMax>();
    if (rawPositionMin == rawPositionMax && rawPositionMin == 0)
    {
        min.position = -base::infinity<double>();
        max.position = base::infinity<double>();
    }
    else
    {
        min.position = mFactors.rawToEncoder(rawPositionMin);
        max.position = mFactors.rawToEncoder(rawPositionMax);
    }

    int32_t rawMaxSpeed = getRaw<MaxMotorSpeed>();
    if (rawMaxSpeed < 0)
    {
        min.speed = -base::infinity<double>();
        max.speed = base::infinity<double>();
    }
    else
    {
        double speedLimit = mFactors.rawToEncoder(rawMaxSpeed);
        min.speed = -speedLimit;
        max.speed = speedLimit;
    }

    min.acceleration = -base::infinity<double>();
    max.acceleration = base::infinity<double>();

    auto torqueAndCurrentLimit = getRaw<MaxCurrent>();
    double torqueLimit = mFactors.rawToTorque(torqueAndCurrentLimit);
    min.effort = -torqueLimit;
    max.effort = torqueLimit;

    double currentLimit = mFactors.rawToCurrent(torqueAndCurrentLimit);
    min.raw = -currentLimit;
    max.raw = currentLimit;

    base::JointLimitRange range;
    range.min = min;
    range.max = max;
    return range;
}

struct PDOMapping : canopen_master::PDOMapping
{
    template<typename Object>
    void add()
    {
        canopen_master::PDOMapping::add(
            Object::OBJECT_ID, Object::OBJECT_SUB_ID, sizeof(typename Object::OBJECT_TYPE));
    }
};

void Controller::setControlTargets(base::JointState const& targets)
{
    if (targets.hasPosition())
    {
        int64_t raw = mFactors.rawFromEncoder(targets.position);
        setRaw<TargetPosition>(raw);
    }
    if (targets.hasSpeed())
    {
        int64_t raw = mFactors.rawFromEncoder(targets.speed);
        setRaw<TargetVelocity>(raw);
    }
    if (targets.hasEffort())
    {
        int64_t raw = mFactors.rawFromTorque(targets.effort);
        setRaw<TargetTorque>(raw);
    }
}

canbus::Message Controller::getRPDOMessage(unsigned int pdoIndex)
{
    return mCanOpen.getRPDOMessage(pdoIndex);
}

std::vector<canbus::Message> Controller::configureControlPDO(
    int pdoIndex, base::JointState::MODE control_mode,
    canopen_master::PDOCommunicationParameters parameters)
{
    PDOMapping mapping;
    switch(control_mode) {
        case base::JointState::POSITION:
            mapping.add<TargetPosition>();
            break;
        case base::JointState::SPEED:
            mapping.add<TargetVelocity>();
            break;
        case base::JointState::EFFORT:
            mapping.add<TargetTorque>();
            break;
        default:
            throw std::invalid_argument("expected control_mode to be POSITION, SPEED or EFFORT");
    }

    auto msg = mCanOpen.configurePDO(false, pdoIndex, parameters, mapping);
    mCanOpen.declareRPDOMapping(pdoIndex, mapping);
    return msg;
}

std::vector<canbus::Message> Controller::configureStatusPDO(
    int pdoIndex, canopen_master::PDOCommunicationParameters parameters)
{
    PDOMapping mapping;
    mapping.add<StatusWord>();
    auto msg = mCanOpen.configurePDO(true, pdoIndex, parameters, mapping);
    mCanOpen.declareTPDOMapping(pdoIndex, mapping);
    return msg;
}

vector<canbus::Message> Controller::configureJointStateUpdatePDOs(
    int pdoIndex, canopen_master::PDOCommunicationParameters parameters, uint64_t fields)
{
    // We need two PDOs only if the three fields are reported. If not, need only
    // one
    PDOMapping mapping0;
    PDOMapping mapping1;
    if (fields == UPDATE_JOINT_STATE) {
        mapping0.add<PositionActualInternalValue>();
        mapping0.add<VelocityActualValue>();
        mapping1.add<CurrentActualValue>();
    }
    else {
        if (fields & UPDATE_JOINT_POSITION)
            mapping0.add<PositionActualInternalValue>();
        if (fields & UPDATE_JOINT_VELOCITY)
            mapping0.add<VelocityActualValue>();
        if (fields & UPDATE_JOINT_CURRENT)
            mapping0.add<CurrentActualValue>();
    }

    vector<canbus::Message> messages;
    if (mapping0.empty()) {
	messages.push_back(mCanOpen.disablePDO(true, pdoIndex));
    }
    else {
        auto pdo = mCanOpen.configurePDO(true, pdoIndex, parameters, mapping0);
        mCanOpen.declareTPDOMapping(pdoIndex, mapping0);
        messages.insert(messages.end(), pdo.begin(), pdo.end());
    }
    if (mapping1.empty()) {
	messages.push_back(mCanOpen.disablePDO(true, pdoIndex + 1));
    }
    else {
        auto pdo = mCanOpen.configurePDO(true, pdoIndex + 1, parameters, mapping1);
        mCanOpen.declareTPDOMapping(pdoIndex + 1, mapping1);
        messages.insert(messages.end(), pdo.begin(), pdo.end());
    }
    return messages;
}

canbus::Message Controller::querySave()
{
    uint8_t buffer[4] = { 's', 'a', 'v', 'e' };
    return mCanOpen.download(0x1010, 1, buffer, 4);
}

canbus::Message Controller::queryLoad()
{
    uint8_t buffer[4] = { 'l', 'o', 'a', 'd' };
    return mCanOpen.download(0x1011, 1, buffer, 4);
}
