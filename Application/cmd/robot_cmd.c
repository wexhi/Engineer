/**
 * @file robot_cmd.c
 * @author your name (you@domain.com)
 * @brief 机器人核心控制任务
 * @attention 因为底盘接的是遥控器，但是云台需要进行视觉传输，因此在预编译时不应该屏蔽掉RobotCMDInit，
 *             否则会出现内存访问错误，应在该文件中修改预编译指令。
 *             由于底盘板和云台板的任务都包含有云台电机的任务，因此应该在此处进行双板之间的通信。
 * @version 0.1
 * @date 2024-01-15
 *
 * @copyright Copyright (c) 2024
 *
 */
// application layer for robot command
#include "robot_cmd.h"
#include "robot_def.h"

// module layer
#include "remote.h"
#include "miniPC_process.h"
#include "VideoTransmitter.h"
#include "message_center.h"
#include "user_lib.h"
#include "miniPC_process.h"
#include "referee_protocol.h"
#include "scara_kinematics.h"
#include "arm_math.h"
#include "UARTComm.h"
// #ifdef CHASSIS_BOARD
// static Publisher_t *chassis_cmd_pub;   // 底盘控制消息发布者
// static Subscriber_t *chassis_feed_sub; // 底盘反馈信息订阅者
// #endif

static float rc_mode_xy[2]             = {0, 0}; // x,y坐标
static float rc_mode_xy_after_check[2] = {0, 0};

static Publisher_t *arm_cmd_pub;         // 底盘控制消息发布者
static Subscriber_t *arm_feed_sub;       // 底盘反馈信息订阅者
static Vision_Recv_s *vision_ctrl;       // 视觉控制信息
static UARTComm_Instance *cmd_uart_comm; // 双板通信

static Chassis_Ctrl_Cmd_s chassis_cmd_send;       // 发送给底盘应用的信息,包括控制信息和UI绘制相关
static Chassis_Upload_Data_s chassis_fetch_data;  // 从底盘应用接收的反馈信息信息,底盘功率枪口热量与底盘运动状态等
static Arm_Ctrl_Cmd_s arm_cmd_send;               // 发送给机械臂的信息
__unused static Arm_Upload_Data_s arm_fetch_data; // 从机械臂接受的反馈信息
static void RemoteControlSet(void);               // 遥控器控制量设置
static void VideoControlSet(void);                // 图传链路控制量设置
static void EmergencyHandler(void);
static RC_ctrl_t *rc_data;       // 遥控器数据指针,初始化时返回
static Video_ctrl_t *video_data; // 视觉数据指针,初始化时返回

/**
 * @brief 机器人核心控制任务初始化,会被RobotInit()调用
 *
 * @attention 工程机器人使用两块板子，它们之间的数据使用UART 1进行通信
 *
 */
void RobotCMDInit(void)
{

    UARTComm_Init_Config_s ucomm_config = {
        .uart_handle    = &huart1,
        .recv_data_len  = sizeof(Chassis_Upload_Data_s),
        .send_data_len  = sizeof(Chassis_Ctrl_Cmd_s),
        .daemon_counter = 10,
    };
    cmd_uart_comm = UARTCommInit(&ucomm_config);
    video_data    = VideoTransmitterControlInit(&huart6); // 初始化图传链路
    rc_data       = RemoteControlInit(&huart3);           // 初始化遥控器,C板上使用USART1
    vision_ctrl   = VisionInit(&huart3);                  // 初始化视觉控制

    arm_cmd_pub  = PubRegister("arm_cmd", sizeof(Arm_Ctrl_Cmd_s));
    arm_feed_sub = SubRegister("arm_feed", sizeof(Arm_Upload_Data_s));

    // 此处初始化与视觉的通信
}

/* 机器人核心控制任务,200Hz频率运行(必须高于视觉发送频率) */
void RobotCMDTask(void)
{
    // 获取机械臂反馈信息
    SubGetMessage(arm_feed_sub, &arm_fetch_data);
    chassis_fetch_data = *(Chassis_Upload_Data_s *)UARTCommGet(cmd_uart_comm);

    if (!rc_data[TEMP].rc.switch_right ||
        switch_is_down(rc_data[TEMP].rc.switch_right)) // 当收不到遥控器信号时，使用图传链路
    {
        VideoControlSet();
    } else if (switch_is_mid(rc_data[TEMP].rc.switch_right) || switch_is_up(rc_data[TEMP].rc.switch_right)) // 当收到遥控器信号时,且右拨杆为中，使用遥控器
    {
        RemoteControlSet();
    }

    // 发送控制信息
    chassis_cmd_send.arm_mode    = arm_cmd_send.arm_mode;
    chassis_cmd_send.sucker_mode = arm_cmd_send.sucker_mode;
    chassis_cmd_send.arm_status  = arm_cmd_send.arm_status;
    chassis_cmd_send.max_arm     = arm_fetch_data.maximal_arm;
    chassis_cmd_send.min_arm     = arm_fetch_data.minimal_arm;

    // 发送给机械臂
    PubPushMessage(arm_cmd_pub, &arm_cmd_send);
    UARTCommSend(cmd_uart_comm, (void *)&chassis_cmd_send);
}
static void SuckerContorl2(void);
/**
 * @brief 控制输入为遥控器(调试时)的模式和控制量设置
 *
 */
static float yaw_offset = 0;
static void RemoteControlSet(void)
{
    arm_cmd_send.arm_mode = ARM_HUM_CONTORL;
    float res_scara_angle[2]; //第一个为大臂，第二个为小臂
    uint8_t switch_left_down_flag;
    uint8_t switch_left_up_flag;
    if (switch_is_down(rc_data[TEMP].rc.switch_left)) {
        switch_left_down_flag = 1;
        switch_left_up_flag   = 0;
    } else if (switch_is_up(rc_data[TEMP].rc.switch_left)) {
        switch_left_down_flag = 0;
        switch_left_up_flag   = 1;
    } else {
        switch_left_down_flag = 0;
        switch_left_up_flag   = 0;
    }

    //传送带
    if (rc_data[TEMP].key[KEY_PRESS].ctrl && rc_data[TEMP].key[KEY_PRESS].z) {
        chassis_cmd_send.trans_mode = TRANS_DIRECT;
    } else if (rc_data[TEMP].key[KEY_PRESS].ctrl && rc_data[TEMP].key[KEY_PRESS].c) {
        chassis_cmd_send.trans_mode = TRANS_REVERSE;
    } else {
        chassis_cmd_send.trans_mode = TRANS_STOP;
    }
    //切取银矿臂形
    if ((rc_data[TEMP].key[KEY_PRESS].shift) && (rc_data[TEMP].key[KEY_PRESS].x)) {
        rc_mode_xy[0]          = 250;
        arm_cmd_send.pitch_arm = -PI / 2;
    }
    //切取金矿臂形
    if ((rc_data[TEMP].key[KEY_PRESS].shift) && (rc_data[TEMP].key[KEY_PRESS].c)) {
        rc_mode_xy[1]          = 0;
        arm_cmd_send.pitch_arm = 0;
        yaw_offset             = 0;
    }
    //前两轴解算部分
    if (!(rc_data[TEMP].key[KEY_PRESS].shift)) {
        rc_mode_xy[1] -= rc_data[TEMP].mouse.x / 50;
        rc_mode_xy[0] -= rc_data[TEMP].mouse.y / 30;
    }
    if (rc_data[TEMP].rc.rocker_r1 > 100) {
        rc_mode_xy[0] += (rc_data[TEMP].rc.rocker_r1) / 240;
    }
    if (rc_data[TEMP].rc.rocker_r1 < -100) {
        rc_mode_xy[0] += (rc_data[TEMP].rc.rocker_r1) / 240;
    }
    if (rc_data[TEMP].rc.rocker_r_ > 100) {
        rc_mode_xy[1] -= (rc_data[TEMP].rc.rocker_r_) / 240;
    }
    if (rc_data[TEMP].rc.rocker_r_ < -100) {
        rc_mode_xy[1] -= (rc_data[TEMP].rc.rocker_r_) / 240;
    }
    // x限位
    if (rc_mode_xy[0] > ARMLENGHT1 + ARMLENGHT2) {
        rc_mode_xy[0] = ARMLENGHT1 + ARMLENGHT2;
    }
    if (rc_mode_xy[0] < 0) {
        rc_mode_xy[0] = 0;
    }
    // y限位
    if (rc_mode_xy[1] > 400) {
        rc_mode_xy[1] = 400;
    }
    if (rc_mode_xy[1] < -228) {
        rc_mode_xy[1] = -228;
    }
    check_boundary_scara_lefthand(rc_mode_xy[0], rc_mode_xy[1], rc_mode_xy_after_check);
    scara_inverse_kinematics(rc_mode_xy_after_check[0], rc_mode_xy_after_check[1], ARMLENGHT1, ARMLENGHT2, 2, res_scara_angle);
    arm_cmd_send.maximal_arm = res_scara_angle[0];
    arm_cmd_send.minimal_arm = res_scara_angle[1];
    //末端三轴和抬升部分
    // yaw pitch
    if ((!(rc_data[TEMP].mouse.press_r && rc_data[TEMP].mouse.press_l && rc_data[TEMP].key[KEY_PRESS].shift)) && (rc_data[TEMP].key[KEY_PRESS].ctrl)) {
        if (rc_data[TEMP].mouse.x > 10) {
            yaw_offset -= 0.01;
        }
        if (rc_data[TEMP].mouse.x < -10) {
            yaw_offset += 0.01;
        }
        if (rc_data[TEMP].mouse.y > 10) {
            arm_cmd_send.pitch_arm -= 0.01;
        }
        if (rc_data[TEMP].mouse.y < -10) {
            arm_cmd_send.pitch_arm += 0.01;
        }
    }
    if (yaw_offset > PI) {
        yaw_offset = PI;
    }
    if (yaw_offset < -PI) {
        yaw_offset = -PI;
    }
    if (arm_cmd_send.pitch_arm < -PI / 2) {
        arm_cmd_send.pitch_arm = -PI / 2;
    }
    if (arm_cmd_send.pitch_arm > PI / 2) {
        arm_cmd_send.pitch_arm = PI / 2;
    }

    arm_cmd_send.finesse = yaw_offset - res_scara_angle[0] - res_scara_angle[1];
    //抬升
    if (rc_data[TEMP].mouse.press_l || rc_data[TEMP].mouse.press_r || switch_left_down_flag || switch_left_up_flag) {
        arm_cmd_send.lift_mode = LIFT_SPEED_MODE;
    } else {
        arm_cmd_send.lift_mode = LIFT_KEEP;
    }
    if (rc_data[TEMP].mouse.press_l && (!rc_data[TEMP].mouse.press_r)) {
        arm_cmd_send.lift += 300;
    } else if (rc_data[TEMP].mouse.press_r && (!rc_data[TEMP].mouse.press_l)) {
        arm_cmd_send.lift -= 300;
    } else {
        arm_cmd_send.lift = 0;
    }
    if (switch_left_down_flag) {
        arm_cmd_send.lift = 10000;
    }
    if (switch_left_up_flag) {
        arm_cmd_send.lift = -10000;
    }
    if (arm_cmd_send.lift > 30000) {
        arm_cmd_send.lift = 30000;
    }
    if (arm_cmd_send.lift < -30000) {
        arm_cmd_send.lift = -30000;
    }
    // roll
    if (rc_data[TEMP].key[KEY_PRESS].q || rc_data[TEMP].key[KEY_PRESS].e) {
        arm_cmd_send.roll_mode = ROLL_ANGLE_MODE;
    } else {
        arm_cmd_send.roll_mode = ROLL_KEEP;
    }
    arm_cmd_send.roll = ((50.f * rc_data[TEMP].key[KEY_PRESS].e) - (50.f * rc_data[TEMP].key[KEY_PRESS].q)) + arm_fetch_data.roll;
    //吸盘，遥控右拨杆上为开
    if ((!(rc_data[TEMP].key[KEY_PRESS].r)) && (switch_is_up(rc_data[TEMP].rc.switch_right))) {
        arm_cmd_send.sucker_mode = SUCKER_ON;
    } else {
        arm_cmd_send.sucker_mode = SUCKER_OFF;
    }
    //平移缓启动
    if ((rc_data[TEMP].key[KEY_PRESS].d && (!rc_data[TEMP].key[KEY_PRESS].a)) || (rc_data[TEMP].rc.rocker_l_ > 200)) {
        chassis_cmd_send.vx += 30;
    }
    if ((rc_data[TEMP].key[KEY_PRESS].a && (!rc_data[TEMP].key[KEY_PRESS].d)) || (rc_data[TEMP].rc.rocker_l_ < -200)) {
        chassis_cmd_send.vx -= 30;
    }
    if ((!rc_data[TEMP].key[KEY_PRESS].a) && (!rc_data[TEMP].key[KEY_PRESS].d) && (fabs(rc_data[TEMP].rc.rocker_l_) < 200)) {
        chassis_cmd_send.vx = 0;
    }
    if (chassis_cmd_send.vx > 7000) {
        chassis_cmd_send.vx = 7000;
    }
    if (chassis_cmd_send.vx < -7000) {
        chassis_cmd_send.vx = -7000;
    }
    //前进缓启动
    int velocity_rate = 1;
    // fast启动!
    if (rc_data[TEMP].key[KEY_PRESS].f) {
        velocity_rate = 4;
    }
    if ((rc_data[TEMP].key[KEY_PRESS].w && (!rc_data[TEMP].key[KEY_PRESS].s)) || (rc_data[TEMP].rc.rocker_l1 > 200)) {
        chassis_cmd_send.vy += 300 * velocity_rate;
    }
    if ((rc_data[TEMP].key[KEY_PRESS].s && (!rc_data[TEMP].key[KEY_PRESS].w)) || (rc_data[TEMP].rc.rocker_l1 < -200)) {
        chassis_cmd_send.vy -= 300 * velocity_rate;
    }
    if ((!rc_data[TEMP].key[KEY_PRESS].s) && (!rc_data[TEMP].key[KEY_PRESS].w) && (fabs(rc_data[TEMP].rc.rocker_l1) < 200)) {
        chassis_cmd_send.vy = 0;
    }
    if (chassis_cmd_send.vy > 15000 * velocity_rate) {
        chassis_cmd_send.vy = 15000 * velocity_rate;
    }
    if (chassis_cmd_send.vy < -15000 * velocity_rate) {
        chassis_cmd_send.vy = -15000 * velocity_rate;
    }
    if (rc_data[TEMP].mouse.press_r && rc_data[TEMP].mouse.press_l && rc_data[TEMP].key[KEY_PRESS].shift) {
        chassis_cmd_send.wz = (float)rc_data[TEMP].mouse.x * 160;
    } else if (fabs(rc_data[TEMP].rc.dial) < 200) {
        chassis_cmd_send.wz = 0;
    } else {
        chassis_cmd_send.wz = -10.0f * (float)rc_data[TEMP].rc.dial;
    }
    //图传角度
    if (rc_data[TEMP].key_count[KEY_PRESS][Key_V] % 2 == 1) {
        arm_cmd_send.video_angle = PITCH_120;
    } else {
        arm_cmd_send.video_angle = PITCH_90;
    }
    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;
}

/**
 * @brief 保持机械臂当前位置不变
 *
 */
static void ArmKeep(void)
{
    arm_cmd_send.maximal_arm = arm_fetch_data.maximal_arm;
    arm_cmd_send.minimal_arm = arm_fetch_data.minimal_arm;
    arm_cmd_send.finesse     = arm_fetch_data.finesse;
    arm_cmd_send.pitch_arm   = arm_fetch_data.pitch_arm;
    arm_cmd_send.lift        = arm_fetch_data.height;
    arm_cmd_send.roll        = arm_fetch_data.roll;
    arm_cmd_send.lift_mode   = LIFT_KEEP;
    arm_cmd_send.roll_mode   = ROLL_KEEP;
    arm_cmd_send.arm_status  = ARM_NORMAL;
}

/**
 * @brief 一键回收模式调用函数
 *
 */
static void Recycle(void)
{

    arm_cmd_send.maximal_arm = -0.5589f;
    arm_cmd_send.minimal_arm = 2.4730f;
    arm_cmd_send.finesse     = 1.2339f;
    arm_cmd_send.pitch_arm   = 0.5509f;
    arm_cmd_send.lift        = -5;
    arm_cmd_send.lift_mode   = 1;
    arm_cmd_send.arm_status  = ARM_RECYCLE;
    // arm_cmd_send.roll = arm_fetch_data.roll;
    // arm_cmd_send.lift = arm_fetch_data.height;
}

/**
 * @brief 从车上取矿
 *
 */
static void GetRockFromCar(void)
{
    arm_cmd_send.maximal_arm = 1.2956f;
    arm_cmd_send.minimal_arm = 2.1921f;
    arm_cmd_send.finesse     = -0.1894f;
    arm_cmd_send.pitch_arm   = -1.6172f;
    arm_cmd_send.lift        = -video_data[TEMP].cus.height + arm_fetch_data.height;
    if (video_data[TEMP].key_data.left_button_down) {
        arm_cmd_send.lift = -25000 + arm_fetch_data.height;
    }
    if (video_data[TEMP].key_data.right_button_down) {
        arm_cmd_send.lift = 25000 + arm_fetch_data.height;
    }
    arm_cmd_send.roll       = -video_data[TEMP].cus.roll_arm_target * 57.3f * 10;
    arm_cmd_send.lift_mode  = LIFT_ANGLE_MODE;
    arm_cmd_send.roll_mode  = ROLL_ANGLE_MODE;
    arm_cmd_send.arm_status = ARM_GETCARROCK;
}
/**
 * @brief 从车上取矿2,取矿后取出,避免矿石碰到 车
 *
 */
static void GetRockFromCar2(void)
{
    arm_cmd_send.maximal_arm = -0.2244f;
    arm_cmd_send.minimal_arm = 2.3997f;
    arm_cmd_send.finesse     = 0.0570f;
    arm_cmd_send.pitch_arm   = -1.610f;
    arm_cmd_send.lift        = -video_data[TEMP].cus.height + arm_fetch_data.height;
    arm_cmd_send.roll        = -video_data[TEMP].cus.roll_arm_target * 57.3f * 10;
    arm_cmd_send.lift_mode   = LIFT_ANGLE_MODE;
    arm_cmd_send.roll_mode   = ROLL_ANGLE_MODE;
    arm_cmd_send.arm_status  = ARM_GETCARROCK2;
}

static void DebugModeControl(void)
{
    switch (video_data[TEMP].key_count[KEY_PRESS_WITH_CTRL][Key_D] % 2) {
        case 0:
            arm_cmd_send.download_mode = DOWNLOAD_OFF;
            break;
        default:
            arm_cmd_send.download_mode = DOWNLOAD_ON;
            break;
    }
}

/**
 * @brief 吸盘控制
 *
 */
static void SuckerContorl(void)
{
    switch (video_data[TEMP].key_count[KEY_PRESS][Key_R] % 2) {
        case 1:
            arm_cmd_send.sucker_mode = SUCKER_ON;
            break;
        default:
            arm_cmd_send.sucker_mode = SUCKER_OFF;
            break;
    }
}

static void SuckerContorl2(void)
{
    switch (rc_data[TEMP].key_count[KEY_PRESS][Key_R] % 2) {
        case 1:
            arm_cmd_send.sucker_mode = SUCKER_ON;
            break;
        default:
            arm_cmd_send.sucker_mode = SUCKER_OFF;
            break;
    }
}

/**
 * @brief 一键取矿模式，根据按钮下的按键数量判断机械臂应该达到的位置
 *
 */
static void VideoAutoGet(void)
{
    arm_cmd_send.arm_mode = ARM_AUTO_CONTORL;
    if (arm_cmd_send.arm_mode != arm_cmd_send.arm_mode_last) {
        ArmKeep();
        video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] = 0; // 清零，确保每次都从初始状态开始
    }
    switch (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] % 2) {
        case 0:
            Recycle();
            break;
        default:
            arm_cmd_send.maximal_arm = 0.f;
            arm_cmd_send.minimal_arm = -0.1771f;
            arm_cmd_send.finesse     = -0.5460f;
            arm_cmd_send.pitch_arm   = 0.696f;
            break;
    }

    if (video_data[TEMP].key_data.left_button_down || video_data[TEMP].key_data.right_button_down) {
        arm_cmd_send.lift_mode = LIFT_ANGLE_MODE;
    } else {
        arm_cmd_send.lift_mode = LIFT_KEEP;
    }
    arm_cmd_send.lift = (3 * (video_data[TEMP].key_data.left_button_down) - 6 * (video_data[TEMP].key_data.right_button_down)) * 10 + arm_fetch_data.height;

    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;
}

/**
 * @brief 键盘控制模式
 *
 */
static void VideoKey(void)
{
    arm_cmd_send.arm_mode = ARM_KEY_CONTROL;
    if (arm_cmd_send.arm_mode != arm_cmd_send.arm_mode_last) {
        ArmKeep();
        video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] = 0; // 清零，确保每次都从初始状态开始
    }
    switch (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] % 2) {
        case 0:
            arm_cmd_send.maximal_arm += (video_data[TEMP].key[KEY_PRESS].q - video_data[TEMP].key[KEY_PRESS].e) * 0.0015f;
            arm_cmd_send.minimal_arm += (video_data[TEMP].key[KEY_PRESS].a - video_data[TEMP].key[KEY_PRESS].d) * 0.003f;
            arm_cmd_send.finesse += (video_data[TEMP].key[KEY_PRESS].z - video_data[TEMP].key[KEY_PRESS].c) * 0.003f;
            arm_cmd_send.pitch_arm += (video_data[TEMP].key[KEY_PRESS].w - video_data[TEMP].key[KEY_PRESS].s) * 0.003f;
            arm_cmd_send.arm_status = ARM_NORMAL;
            break;
        default:
            GetRockFromCar();
            break;
    }
    if (video_data[TEMP].key_data.left_button_down || video_data[TEMP].key_data.right_button_down) {
        arm_cmd_send.lift_mode = LIFT_ANGLE_MODE;
    } else {
        arm_cmd_send.lift_mode = LIFT_KEEP;
    }
    arm_cmd_send.lift = (3 * (video_data[TEMP].key_data.left_button_down) - 6 * (video_data[TEMP].key_data.right_button_down)) * 10 + arm_fetch_data.height;

    if (video_data[TEMP].key[KEY_PRESS].f || video_data[TEMP].key[KEY_PRESS].g) {
        arm_cmd_send.roll_mode = ROLL_ANGLE_MODE;
    } else {
        arm_cmd_send.roll_mode = ROLL_KEEP;
    }
    arm_cmd_send.roll = ((10.f * video_data[TEMP].key[KEY_PRESS].f) - (10.f * video_data[TEMP].key[KEY_PRESS].g)) + arm_fetch_data.roll;

    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;
}

/**
 * @brief 自定义控制器正常模式控制
 *
 */
static void VideoCustom(void)
{
    // float angle_ref[6];
    // static float PushLength;
    // float temp_xy[2];
    arm_cmd_send.arm_mode = ARM_HUM_CONTORL;
    if (arm_cmd_send.arm_mode != arm_cmd_send.arm_mode_last) {
        ArmKeep();
        video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] = 0; // 清零，确保每次都从初始状态开始
    }
    switch (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] % 3) {
        case 0:
            arm_cmd_send.maximal_arm = video_data[TEMP].cus.maximal_arm_target;
            arm_cmd_send.minimal_arm = video_data[TEMP].cus.minimal_arm_target;
            arm_cmd_send.finesse     = video_data[TEMP].cus.finesse_target * 1.2;
            arm_cmd_send.pitch_arm   = video_data[TEMP].cus.pitch_arm_target;
            arm_cmd_send.lift        = -video_data[TEMP].cus.height + arm_fetch_data.height;
            arm_cmd_send.roll        = -video_data[TEMP].cus.roll_arm_target * 57.3f * 5;
            arm_cmd_send.lift_mode   = LIFT_ANGLE_MODE;
            arm_cmd_send.roll_mode   = ROLL_ANGLE_MODE;
            arm_cmd_send.arm_status  = ARM_NORMAL;

            if (video_data[TEMP].key_data.left_button_down) {
                arm_cmd_send.lift = -25000 + arm_fetch_data.height;
            }
            if (video_data[TEMP].key_data.right_button_down) {
                arm_cmd_send.lift = 25000 + arm_fetch_data.height;
            }
            // 保持吸盘位置高度
            // arm_cmd_send.lift += sin(arm_fetch_data.pitch_arm - PITCH_ZERO_POINT) * ARMLENGHT3;
            // if (video_data[TEMP].key[KEY_PRESS].c) {
            //     if (video_data[LAST].key[KEY_PRESS].c == 0) {
            //         PushLength = 0;
            //         GetCurrentState(arm_fetch_data.maximal_arm, arm_fetch_data.minimal_arm, arm_fetch_data.finesse, arm_fetch_data.pitch_arm, arm_fetch_data.height, arm_fetch_data.roll);
            //     }
            //     PushLength += 0.1;
            //     PushToCube(angle_ref, PushLength);
            //     arm_cmd_send.maximal_arm = angle_ref[0];
            //     arm_cmd_send.minimal_arm = angle_ref[1];
            //     arm_cmd_send.finesse     = angle_ref[2];
            //     arm_cmd_send.pitch_arm   = angle_ref[3];
            //     arm_cmd_send.lift        = angle_ref[5];
            //     arm_cmd_send.roll        = angle_ref[4];
            //     arm_cmd_send.lift_mode   = LIFT_ANGLE_MODE;
            //     arm_cmd_send.roll_mode   = ROLL_ANGLE_MODE;
            //     arm_cmd_send.arm_status  = ARM_NORMAL;
            // }
            break;
        case 1:
            GetRockFromCar();
            break;
        default:
            GetRockFromCar2();
            break;
    }

    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;
}

/**
 * @brief 视觉控制
 *
 */
static void VisionContorl(void)
{
    arm_cmd_send.arm_mode = ARM_VISION_CONTROL;
    if (arm_cmd_send.arm_mode != arm_cmd_send.arm_mode_last) {
        VisionSend(1);
    }
    if (video_data[TEMP].key[KEY_PRESS].v) { // 按下V键向视觉发送数据
        VisionSend(1);
    } else {
        VisionSend(0);
    }
    if (vision_ctrl->is_tracking) {
        arm_cmd_send.maximal_arm = vision_ctrl->maximal_arm + MAXARM_ZERO;
        arm_cmd_send.minimal_arm = vision_ctrl->minimal_arm + MINARM_ZERO;
        arm_cmd_send.finesse     = vision_ctrl->finesse + FINE_ZERO;
        arm_cmd_send.pitch_arm   = vision_ctrl->pitch_arm + PITCH_ZERO;
    } else {
        ArmKeep();
    }

    if (video_data[TEMP].key_data.left_button_down || video_data[TEMP].key_data.right_button_down) {
        arm_cmd_send.lift_mode = LIFT_ANGLE_MODE;
    }
    arm_cmd_send.lift = (3 * (video_data[TEMP].key_data.left_button_down) - 6 * (video_data[TEMP].key_data.right_button_down)) * 10 + arm_fetch_data.height;
    if (video_data[TEMP].key[KEY_PRESS].f || video_data[TEMP].key[KEY_PRESS].g) {
        arm_cmd_send.roll_mode = ROLL_ANGLE_MODE;
    } else {
        arm_cmd_send.roll_mode = ROLL_KEEP;
    }
    arm_cmd_send.roll          = arm_fetch_data.roll + 5 * (video_data[TEMP].key[KEY_PRESS].f - video_data[TEMP].key[KEY_PRESS].g);
    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;
}

/**
 * @brief 自定义控制器的微调模式
 *
 */
static void VideoSlightlyContorl(void)
{
    arm_cmd_send.arm_mode = ARM_SLIGHTLY_CONTROL;
    static float remenber_finesse_angle;
    if (arm_cmd_send.arm_mode != arm_cmd_send.arm_mode_last) {
        StateInit(arm_fetch_data.maximal_arm, arm_fetch_data.minimal_arm, arm_fetch_data.finesse, arm_fetch_data.pitch_arm, arm_fetch_data.height, 0);
        arm_cmd_send.arm_status = ARM_NORMAL;
        remenber_finesse_angle  = arm_fetch_data.maximal_arm + arm_fetch_data.minimal_arm + arm_fetch_data.finesse;
    }

    float angle_ref[6];
    // 轻微控制器数据
    GC_get_target_angles_slightly(video_data[TEMP].scd, angle_ref);
    arm_cmd_send.maximal_arm = angle_ref[0];
    arm_cmd_send.minimal_arm = angle_ref[1];
    arm_cmd_send.finesse     = remenber_finesse_angle - arm_fetch_data.maximal_arm - arm_fetch_data.minimal_arm;
    // 不要pitch和和yaw的控制量了
    arm_cmd_send.roll          = angle_ref[4];
    arm_cmd_send.lift          = video_data[TEMP].scd.delta_z + arm_fetch_data.height; // 微调模式与全控制模式相同
    arm_cmd_send.lift_mode     = LIFT_ANGLE_MODE;
    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;
}

__attribute__((used)) static void VideoHeigtInit(void)
{
    arm_cmd_send.arm_mode = ARM_LIFT_INIT;
    switch (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] % 2) {
        case 0:
            if (video_data[TEMP].key_data.right_button_down) {
                arm_cmd_send.lift_mode = LIFT_INIT_MODE;
                arm_cmd_send.lift_init = 1;
            } else {
                arm_cmd_send.lift_mode = LIFT_KEEP;
                arm_cmd_send.lift_init = 0;
            }
            break;
        default:
            if (video_data[TEMP].cus.height > 0) {
                arm_cmd_send.lift_mode = LIFT_INIT_MODE;
                arm_cmd_send.lift_init = 1;
            } else {
                arm_cmd_send.lift_mode = LIFT_KEEP;
                arm_cmd_send.lift_init = 0;
            }
            break;
    }

    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;
}

/**
 * @brief 图传链路以及自定义控制器的模式和控制量设置
 *
 */
#define ARM_MODE_COUNT 6
static int mode;
static void VideoControlSet(void)
{
    arm_cmd_send.arm_mode = ARM_HUM_CONTORL;
    float res_scara_angle[2]; //第一个为大臂，第二个为小臂
    uint8_t switch_left_down_flag;
    uint8_t switch_left_up_flag;
    // if (switch_is_down(rc_data[TEMP].rc.switch_left)) {
    //     switch_left_down_flag = 1;
    //     switch_left_up_flag   = 0;
    // } else if (switch_is_up(rc_data[TEMP].rc.switch_left)) {
    //     switch_left_down_flag = 0;
    //     switch_left_up_flag   = 1;
    // } else {
    //     switch_left_down_flag = 0;
    //     switch_left_up_flag   = 0;
    // }
    //取银矿模式
    if (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_X] % 2 == 1) {
        rc_mode_xy[0]          = 250;
        arm_cmd_send.pitch_arm = -PI / 2;
    }
    //取金矿模式
    if (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_C] % 2 == 1) {
        rc_mode_xy[1]          = 0;
        arm_cmd_send.pitch_arm = 0;
        yaw_offset             = 0;
    }
    //前两轴解算部分
    if (!(video_data[TEMP].key[KEY_PRESS].shift)) {
        rc_mode_xy[1] -= video_data[TEMP].key_data.mouse_x / 50;
        rc_mode_xy[0] -= video_data[TEMP].key_data.mouse_y / 30;
    }
    // if (rc_data[TEMP].rc.rocker_r1 > 100) {
    //     rc_mode_xy[0] += (rc_data[TEMP].rc.rocker_r1) / 240;
    // }
    // if (rc_data[TEMP].rc.rocker_r1 < -100) {
    //     rc_mode_xy[0] += (rc_data[TEMP].rc.rocker_r1) / 240;
    // }
    // if (rc_data[TEMP].rc.rocker_r_ > 100) {
    //     rc_mode_xy[1] -= (rc_data[TEMP].rc.rocker_r_) / 240;
    // }
    // if (rc_data[TEMP].rc.rocker_r_ < -100) {
    //     rc_mode_xy[1] -= (rc_data[TEMP].rc.rocker_r_) / 240;
    // }
    // x限位
    if (rc_mode_xy[0] > ARMLENGHT1 + ARMLENGHT2) {
        rc_mode_xy[0] = ARMLENGHT1 + ARMLENGHT2;
    }
    if (rc_mode_xy[0] < 0) {
        rc_mode_xy[0] = 0;
    }
    // y限位
    if (rc_mode_xy[1] > 400) {
        rc_mode_xy[1] = 400;
    }
    if (rc_mode_xy[1] < -228) {
        rc_mode_xy[1] = -228;
    }
    check_boundary_scara_lefthand(rc_mode_xy[0], rc_mode_xy[1], rc_mode_xy_after_check);
    scara_inverse_kinematics(rc_mode_xy_after_check[0], rc_mode_xy_after_check[1], ARMLENGHT1, ARMLENGHT2, 2, res_scara_angle);
    arm_cmd_send.maximal_arm = res_scara_angle[0];
    arm_cmd_send.minimal_arm = res_scara_angle[1];
    //末端三轴和抬升部分
    // yaw pitch
    if ((!(video_data[TEMP].key_data.right_button_down && video_data[TEMP].key_data.left_button_down && video_data[TEMP].key[KEY_PRESS].shift)) && (video_data[TEMP].key[KEY_PRESS].shift)) {
        if (video_data[TEMP].key_data.mouse_x > 10) {
            yaw_offset -= 0.01;
        }
        if (video_data[TEMP].key_data.mouse_x < -10) {
            yaw_offset += 0.01;
        }
        if (video_data[TEMP].key_data.mouse_y > 10) {
            arm_cmd_send.pitch_arm -= 0.01;
        }
        if (video_data[TEMP].key_data.mouse_y < -10) {
            arm_cmd_send.pitch_arm += 0.01;
        }
    }
    if (yaw_offset > PI) {
        yaw_offset = PI;
    }
    if (yaw_offset < -PI) {
        yaw_offset = -PI;
    }
    if (arm_cmd_send.pitch_arm < -PI / 2) {
        arm_cmd_send.pitch_arm = -PI / 2;
    }
    if (arm_cmd_send.pitch_arm > PI / 2) {
        arm_cmd_send.pitch_arm = PI / 2;
    }
    arm_cmd_send.finesse = yaw_offset - res_scara_angle[0] - res_scara_angle[1];
    //放矿模式基础位置
    //摆到放矿位置
    if (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] % 3 == 1) {
        arm_cmd_send.maximal_arm = 1.42956f;
        arm_cmd_send.minimal_arm = 2.01921f;
        arm_cmd_send.finesse     = 0.f + yaw_offset;
        arm_cmd_send.pitch_arm   = -PI / 2;
    }
    if (video_data[TEMP].key_count[KEY_PRESS_WITH_SHIFT][Key_Z] % 3 == 2) {
        arm_cmd_send.maximal_arm = 0.2244f;
        arm_cmd_send.minimal_arm = 2.03997f;
        arm_cmd_send.finesse     = 0.0570f;
        arm_cmd_send.pitch_arm   = -PI / 2;
    }
    //抬升
    if (video_data[TEMP].key_data.left_button_down || video_data[TEMP].key_data.right_button_down || switch_left_down_flag || switch_left_up_flag) {
        // arm_cmd_send.lift_mode = LIFT_ANGLE_MODE;
        arm_cmd_send.lift_mode = LIFT_SPEED_MODE;
    } else {
        arm_cmd_send.lift_mode = LIFT_KEEP;
    }
    if (video_data[TEMP].key_data.left_button_down && (!video_data[TEMP].key_data.right_button_down)) {
        arm_cmd_send.lift += 300;
    } else if (video_data[TEMP].key_data.right_button_down && (!video_data[TEMP].key_data.left_button_down)) {
        arm_cmd_send.lift -= 300;
    } else {
        arm_cmd_send.lift = 0;
    }
    // if (switch_left_down_flag) {
    //     arm_cmd_send.lift = 10000;
    // }
    // if (switch_left_up_flag) {
    //     arm_cmd_send.lift = -10000;
    // }
    if (arm_cmd_send.lift > 30000) {
        arm_cmd_send.lift = 30000;
    }
    if (arm_cmd_send.lift < -30000) {
        arm_cmd_send.lift = -30000;
    }
    // roll
    if (video_data[TEMP].key[KEY_PRESS].q || video_data[TEMP].key[KEY_PRESS].e) {
        arm_cmd_send.roll_mode = ROLL_ANGLE_MODE;
    } else {
        arm_cmd_send.roll_mode = ROLL_KEEP;
    }
    arm_cmd_send.roll = ((30.f * video_data[TEMP].key[KEY_PRESS].e) - (30.f * video_data[TEMP].key[KEY_PRESS].q)) + arm_fetch_data.roll;
    //吸盘，遥控右拨杆上为开
    if ((video_data[TEMP].key_count[KEY_PRESS][Key_R] % 2 == 1)) {
        arm_cmd_send.sucker_mode = SUCKER_ON;
    } else {
        arm_cmd_send.sucker_mode = SUCKER_OFF;
    }
    chassis_cmd_send.chassis_mode = CHASSIS_SLOW; // 底盘模式
    // 底盘参数,目前没有加入小陀螺(调试似乎暂时没有必要),系数需要调整
    // chassis_cmd_send.vx = 20.0f * (float)rc_data[TEMP].rc.rocker_l_; // _水平方向
    // chassis_cmd_send.vy = 20.0f * (float)rc_data[TEMP].rc.rocker_l1; // 1竖直方向
    // chassis_cmd_send.wz = -10.0f * (float)rc_data[TEMP].rc.dial;     // _水平方向
    //平移缓启动
    if (video_data[TEMP].key[KEY_PRESS].d && (!video_data[TEMP].key[KEY_PRESS].a)) {
        chassis_cmd_send.vx += 30;
    }
    if (video_data[TEMP].key[KEY_PRESS].a && (!video_data[TEMP].key[KEY_PRESS].d)) {
        chassis_cmd_send.vx -= 30;
    }
    if ((!video_data[TEMP].key[KEY_PRESS].a) && (!video_data[TEMP].key[KEY_PRESS].d)) {
        chassis_cmd_send.vx = 0;
    }
    if (chassis_cmd_send.vx > 7000) {
        chassis_cmd_send.vx = 7000;
    }
    if (chassis_cmd_send.vx < -7000) {
        chassis_cmd_send.vx = -7000;
    }
    //前进缓启动
    if (video_data[TEMP].key[KEY_PRESS].w && (!video_data[TEMP].key[KEY_PRESS].s)) {
        chassis_cmd_send.vy += 300;
    }
    if (video_data[TEMP].key[KEY_PRESS].s && (!video_data[TEMP].key[KEY_PRESS].w)) {
        chassis_cmd_send.vy -= 300;
    }
    if ((!video_data[TEMP].key[KEY_PRESS].s) && (!video_data[TEMP].key[KEY_PRESS].w)) {
        chassis_cmd_send.vy = 0;
    }
    if (chassis_cmd_send.vy > 15000) {
        chassis_cmd_send.vy = 15000;
    }
    if (chassis_cmd_send.vy < -15000) {
        chassis_cmd_send.vy = -15000;
    }
    // chassis_cmd_send.vx = (rc_data[TEMP].key[KEY_PRESS].d - rc_data[TEMP].key[KEY_PRESS].a) * 30000 * 0.5f;
    // chassis_cmd_send.vy = (rc_data[TEMP].key[KEY_PRESS].w - rc_data[TEMP].key[KEY_PRESS].s) * 30000 * 0.5f;
    if (video_data[TEMP].key_data.right_button_down && video_data[TEMP].key_data.left_button_down && video_data[TEMP].key[KEY_PRESS].shift) {
        chassis_cmd_send.wz = (float)video_data[TEMP].key_data.mouse_x * 160;
    } else {
        chassis_cmd_send.wz = 0;
    }
    //图传角度
    if (video_data[TEMP].key_count[KEY_PRESS][Key_V] % 2 == 1) {
        arm_cmd_send.video_angle = PITCH_120;
    } else {
        arm_cmd_send.video_angle = PITCH_90;
    }
    if (video_data[TEMP].key[KEY_PRESS].ctrl && video_data[TEMP].key[KEY_PRESS].z) {
        chassis_cmd_send.trans_mode = TRANS_DIRECT;
    } else if (video_data[TEMP].key[KEY_PRESS].ctrl && video_data[TEMP].key[KEY_PRESS].c) {
        chassis_cmd_send.trans_mode = TRANS_REVERSE;
    } else {
        chassis_cmd_send.trans_mode = TRANS_STOP;
    }
    arm_cmd_send.arm_mode_last = arm_cmd_send.arm_mode;

    // // 机械臂控制
    // switch (video_data[TEMP].key_count[KEY_PRESS][Key_B] % 2) {
    //     case 0:
    //         mode = (video_data[TEMP].key_count[KEY_PRESS_WITH_CTRL][Key_X] -
    //                 video_data[TEMP].key_count[KEY_PRESS_WITH_CTRL][Key_S] + 100 * ARM_MODE_COUNT) %
    //                ARM_MODE_COUNT;
    //         break;
    //     default:
    //         mode = 999;
    //         break;
    // }
    // // if (video_data[TEMP].custom_control_mode == 0) {
    // switch (mode) {
    //     case 0:
    //         VideoKey(); // 键盘控制
    //         // VisionContorl();
    //         break;
    //     case 1:
    //         VideoCustom(); // 自定义控制器
    //         break;
    //     case 2:
    //         VisionContorl(); // 视觉控制
    //         break;
    //     case 3:
    //         VideoSlightlyContorl(); // 轻微控制器
    //         break;
    //     case 999:
    //         VideoAutoGet(); // 一键取矿
    //         break;
    //     // case 5:
    //     //     VideoHeigtInit(); // 高度初始化
    //     //     break;
    //     default:
    //         break;
    // }
    // // } else {
    // //     // 接收到自定义控制器命令，转为自定义控制器拨杆切换模式
    // //     // code ...
    // // }
    // DebugModeControl();
    // SuckerContorl();

    // VAL_LIMIT(arm_cmd_send.maximal_arm, MAXARM_MIN, MAXARM_MAX);
    // VAL_LIMIT(arm_cmd_send.minimal_arm, MINARM_MIN, MINARM_MAX);
    // if (arm_cmd_send.pitch_arm > -0.1f) {
    //     VAL_LIMIT(arm_cmd_send.finesse, FINE_MIN2, FINE_MAX2);
    // } else {
    //     VAL_LIMIT(arm_cmd_send.finesse, FINE_MIN, FINE_MAX);
    // }
    // VAL_LIMIT(arm_cmd_send.pitch_arm, PITCH_MIN, PITCH_MAX);
    // if (arm_cmd_send.lift_mode == LIFT_ANGLE_MODE)
    //     VAL_LIMIT(arm_cmd_send.lift, HEIGHT_MIN, HEIGHT_MAX);
    // VAL_LIMIT(arm_cmd_send.roll, ROLL_MIN, ROLL_MAX);

    // switch (video_data[TEMP].key_count[KEY_PRESS][Key_V] % 2) {
    //     case 0:
    //         arm_cmd_send.video_angle = PITCH_90;
    //         break;
    //     default:
    //         arm_cmd_send.video_angle = PITCH_120;
    //         break;
    // }

    // switch (video_data[TEMP].key_count[KEY_PRESS_WITH_CTRL][Key_C] % 3) {
    //     case 0:
    //         chassis_cmd_send.chassis_mode       = CHASSIS_FAST;
    //         chassis_cmd_send.chassis_speed_buff = 0.51;
    //         break;
    //     case 1:
    //         chassis_cmd_send.chassis_mode       = CHASSIS_SLOW;
    //         chassis_cmd_send.chassis_speed_buff = 0.5;
    //         break;
    //     default:
    //         chassis_cmd_send.chassis_mode = CHASSIS_ZERO_FORCE;
    //         break;
    // }
    // if (video_data[TEMP].key[KEY_PRESS_WITH_CTRL].v) {
    //     chassis_cmd_send.ui_mode = UI_REFRESH;
    // } else {
    //     chassis_cmd_send.ui_mode = UI_KEEP;
    // }

    // if (video_data[TEMP].key[KEY_PRESS].g) {
    //     chassis_cmd_send.trans_mode = TRANS_STOP;
    // }
    // if (video_data[TEMP].key[KEY_PRESS_WITH_SHIFT].g) {
    //     chassis_cmd_send.trans_mode = TRANS_DIRECT;
    // }
    // if (video_data[TEMP].key[KEY_PRESS_WITH_CTRL].g) {
    //     chassis_cmd_send.trans_mode = TRANS_REVERSE;
    // }

    // chassis_cmd_send.vx = (video_data[TEMP].key[KEY_PRESS].d - video_data[TEMP].key[KEY_PRESS].a) * 30000 * chassis_cmd_send.chassis_speed_buff; // 系数待测
    // chassis_cmd_send.vy = (video_data[TEMP].key[KEY_PRESS].w - video_data[TEMP].key[KEY_PRESS].s) * 30000 * chassis_cmd_send.chassis_speed_buff; // 系数待测                                                                                                         // test
    // chassis_cmd_send.wz = (float)video_data[TEMP].key_data.mouse_x * 10 +
    //                       (-video_data[TEMP].key[KEY_PRESS].q + video_data[TEMP].key[KEY_PRESS].e) * 8000 * chassis_cmd_send.chassis_speed_buff;
    // chassis_cmd_send.chassis_speed_buff = 1; // test
}

/**
 * @brief  紧急停止,包括遥控器右侧上侧拨杆打满/重要模块离线/双板通信失效等
 *         停止的阈值'300'待修改成合适的值,或改为开关控制.
 *
 * @todo   后续修改为遥控器离线则电机停止(关闭遥控器急停),通过给遥控器模块添加daemon实现
 *
 */
static void EmergencyHandler(void)
{
    // 底盘急停
    arm_cmd_send.arm_mode         = ARM_ZERO_FORCE;
    chassis_cmd_send.chassis_mode = CHASSIS_ZERO_FORCE;
}
