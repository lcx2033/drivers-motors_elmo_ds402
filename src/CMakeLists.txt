rock_library(motors_elmo_ds402
    SOURCES Objects.cpp Controller.cpp Factors.cpp
    HEADERS Objects.hpp Controller.hpp Factors.hpp Update.hpp MotorParameters.hpp
    DEPS_PKGCONFIG canbus canopen_master)

rock_executable(motors_elmo_ds402_ctl Main.cpp
    DEPS motors_elmo_ds402)
