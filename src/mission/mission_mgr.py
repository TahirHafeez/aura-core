from props import getNode

import comms.events

import mission.task.is_airborne
import mission.task.camera
import mission.task.circle
import mission.task.excite
import mission.task.flaps_mgr
import mission.task.home_mgr
import mission.task.idle
import mission.task.land2
import mission.task.land3
import mission.task.launch
import mission.task.lost_link
import mission.task.mode_mgr
import mission.task.parametric
import mission.task.preflight
import mission.task.calibrate
import mission.task.route
import mission.task.switches
import mission.task.throttle_safety

class MissionMgr:
    def __init__(self):
        self.targets_node = getNode("/autopilot/targets", True)
        self.missions_node = getNode("/config/mission", True)
        self.pos_node = getNode("/position", True)
        self.task_node = getNode("/task", True)
        self.preflight_node = getNode("/task/preflight", True)
        self.circle_node = getNode("/task/circle", True)
        self.home_node = getNode("/task/home", True)
        self.wind_node = getNode("/filters/wind", True)
        self.global_tasks = []
        self.seq_tasks = []
        self.standby_tasks = []

    def make_task(self, config_node):
        result = None
        task_name = config_node.name
        print("  make_task():", task_name)
        if task_name == 'is_airborne':
            result = mission.task.is_airborne.IsAirborne(config_node)
        elif task_name == 'camera':
            result = mission.task.camera.Camera(config_node)
        elif task_name == 'circle':
            result = mission.task.circle.Circle(config_node)
        elif task_name == 'excite':
            result = mission.task.excite.Excite(config_node)
        elif task_name == 'flaps_manager':
            result = mission.task.flaps_mgr.FlapsMgr(config_node)
        elif task_name == 'home_manager':
            result = mission.task.home_mgr.HomeMgr(config_node)
        elif task_name == 'idle':
            result = mission.task.idle.Idle(config_node)
        elif task_name == 'land':
            result = mission.task.land2.Land(config_node)
        elif task_name == 'land3':
            result = mission.task.land3.Land(config_node)
        elif task_name == 'launch':
            result = mission.task.launch.Launch(config_node)
        elif task_name == 'lost_link':
            result = mission.task.lost_link.LostLink(config_node)
        elif task_name == 'mode_manager':
            result = mission.task.mode_mgr.ModeMgr(config_node)
        elif task_name == 'parametric':
            result = mission.task.parametric.Parametric(config_node)
        elif task_name == 'preflight':
            result = mission.task.preflight.Preflight(config_node)
        elif task_name == 'calibrate':
            result = mission.task.calibrate.Calibrate(config_node)
        elif task_name == 'route':
            result = mission.task.route.Route(config_node)
        elif task_name == 'switches':
            result = mission.task.switches.Switches(config_node)
        elif task_name == 'throttle_safety':
            result = mission.task.throttle_safety.ThrottleSafety(config_node)
        else:
            print("mission_mgr: unknown task name:", task_name)
        return result

    def init(self):
        print("global_tasks:")
        global_node = self.missions_node.getChild("global_tasks", True)
        if global_node:
            for name in global_node.getChildren():
                config_node = global_node.getChild(name)
                task = self.make_task(config_node)
                if task != None:
                    self.global_tasks.append( task )

        print("sequential_tasks:")
        seq_node = self.missions_node.getChild("sequential_tasks", True)
        if seq_node:
            for name in seq_node.getChildren():
                config_node = seq_node.getChild(name)
                task = self.make_task(config_node)
                if task != None:
                    self.seq_tasks.append( task )

        print("standby_tasks:")
        standby_node = self.missions_node.getChild("standby_tasks", True)
        if standby_node:
            for name in standby_node.getChildren():
                config_node = standby_node.getChild(name)
                task = self.make_task(config_node)
                if task != None:
                    self.standby_tasks.append( task )

        # activate all the tasks in the global queue
        for task in self.global_tasks:
            task.activate()

        # activate the first task on the sequential queue
        if len(self.seq_tasks):
            self.seq_tasks[0].activate()

    def update(self, dt):
        self.process_command_request()

        # run all tasks in the global queue
        for task in self.global_tasks:
            task.update(dt)

        if len(self.seq_tasks):
            # run the first task in the sequential queue
            task = self.seq_tasks[0]
            self.task_node.setString("current_task_id", task.name)
            task.update(dt)
            if task.is_complete():
                # current task is complete, close it and pop it off the list
                comms.events.log("mission", "task complete: " + task.name)
                # FIXME
                # if ( display_on ) {
                #     printf("task complete: %s\n", front->get_name_cstr())
                # }
                task.close()
                self.pop_seq_task()

                # activate next task if there is one
                if len(self.seq_tasks):
                    task = self.seq_tasks[0]
                    task.activate()
                    comms.events.log("mission", "next task: " + task.name)
        if not len(self.seq_tasks):
            # sequential queue is empty so request the idle task
            self.request_task_idle()
        return True

    def find_global_task(self, name):
        for task in self.global_tasks:
            if task.name == name:
                return task
        return None

    def front_seq_task(self):
        if len(self.seq_tasks):
            return self.seq_tasks[0]
        else:
            return None

    def push_seq_task(self, task):
        self.seq_tasks.insert(0, task)

    def pop_seq_task(self):
        if len(self.seq_tasks):
            task = self.seq_tasks.pop(0)
            task.close()

    def find_seq_task(self, name):
        for task in self.seq_tasks:
            if task.name == name:
                return task
        return None

    def find_standby_task(self, name):
        if name != "":
            for task in self.standby_tasks:
                if task.name == name:
                    return task
        return None

    def find_standby_task_by_nickname(self, nickname):
        if nickname != "":
            for task in self.standby_tasks:
                if task.nickname == nickname:
                    return task
        return None

    def process_command_request(self):
        command = self.task_node.getString("command_request")
        result = "successful: " + command # let's be optimistic!
        if len(command):
            tokens = command.split(",")
            # these commands 'push' a new task onto the front of the
            # sequential task queue (prioritizing over what was
            # previously happening.)  The 'resume' task will pop the
            # task off and resume the original task if it exists.
            if len(tokens) == 2 and tokens[1] == "home":
                self.request_task_home()
            elif len(tokens) == 2 and tokens[1] == "circle":
                self.request_task_circle()
            elif len(tokens) == 4 and tokens[1] == "circle":
                self.request_task_circle( tokens[2], tokens[3] )
            elif len(tokens) == 2 and tokens[1] == "idle":
                self.request_task_idle()
            elif len(tokens) == 2 and tokens[1] == "resume":
                self.request_task_resume()
            elif len(tokens) == 2 and tokens[1] == "land":
                hdg_deg = self.wind_node.getFloat("wind_dir_deg")
                self.request_task_land(hdg_deg)
            elif len(tokens) == 3 and tokens[1] == "land":
                hdg_deg = float(tokens[2])
                self.request_task_land(hdg_deg)
            elif len(tokens) == 3 and tokens[1] == "preflight":
                self.request_task_preflight(tokens[2])
            elif len(tokens) == 2 and tokens[1] == "calibrate":
                self.request_task_calibrate()
            elif len(tokens) == 2 and tokens[1] == "route":
                self.request_task_route()
            elif len(tokens) == 2 and tokens[1] == "parametric":
                self.request_task_parametric()
            elif len(tokens) == 2 and tokens[1] == "pop":
                self.pop_seq_task()
            else:
                result = "syntax error: " + command # bummer
            self.task_node.setString("command_request", "")
            self.task_node.setString("command_result", result)

    def request_task_home(self):
        nickname = "circle_home"
        task = None

        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.nickname == nickname:
                return

        task = self.find_standby_task_by_nickname(nickname)
        if task:
            # activate task
            self.push_seq_task(task)
            task.activate()
        # FIXME: elif display_on:
        #    print "oops, couldn't find 'circle-home' task"


    def request_task_circle(self, lon_deg=None, lat_deg=None):
        lon = 0.0
        lat = 0.0
        if lon_deg == None or lat_deg == None:
            # no coordinates specified, use current position
            lon = self.pos_node.getFloat("longitude_deg")
            lat = self.pos_node.getFloat("latitude_deg")
        else:
            lon = float(lon_deg)
            lat = float(lat_deg)

        nickname = "circle_target"
        task = None

        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.nickname != nickname:
                task = self.find_standby_task_by_nickname( nickname )
                if task:
                    # activate task
                    self.push_seq_task(task)
                    task.activate()

        # setup the target coordinates
        self.circle_node.setFloat( "longitude_deg", lon )
        self.circle_node.setFloat( "latitude_deg", lat )

        # FIXME else if display_on:
        #    print "oops, couldn't find task by nickname:", nickname


    def request_task_circle_descent(self, lon_deg, lat_deg,
                                    radius_m, direction,
                                    exit_agl_ft, exit_heading_deg):
        lon = 0.0
        lat = 0.0
        if lon_deg == None or lat_deg == None:
            # no coordinates specified, use current position
            lon = self.pos_node.getFloat("longitude_deg")
            lat = self.pos_node.getFloat("latitude_deg")
        else:
            lon = float(lon_deg)
            lat = float(lat_deg)

        nickname = "circle_descent"
        task = None

        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.nickname != nickname:
                task = self.find_standby_task_by_nickname( nickname )
                if task:
                    # activate task
                    self.push_seq_task(task)
                    task.activate()

                    # setup the target coordinates
                    self.circle_node.setFloat( "longitude_deg", lon )
                    self.circle_node.setFloat( "latitude_deg", lat )

                    # circle configuration
                    self.circle_node.setFloat("radius_m", radius_m)
                    self.circle_node.setString("direction", direction)

                    # set the exit condition settings
                    task.exit_agl_ft = exit_agl_ft
                    task.exit_heading_deg = exit_heading_deg

        # FIXME else if display_on:
        #    print "oops, couldn't find task by nickname:", nickname

    def request_task_idle(self):
        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.name == "idle":
                return

        task = self.find_standby_task( "idle" )
        if task:
            # activate task
            self.push_seq_task(task)
            task.activate()
        # FIXME else if display_on:
        #    print "oops, couldn't find 'idle' task"

    def request_task_resume(self):
        # look for any 'circle-coord' task at the front of the sequential
        # queue and remove it if it exists
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.name == "circle-coord" or task.name == "land":
                task.close()
                self.pop_seq_task()

    def request_task_preflight(self, duration):
        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.name == "preflight":
                return
        self.preflight_node.setFloat("duration_sec", duration)
        task = self.find_standby_task( "preflight" )
        if task:
            # activate task
            self.push_seq_task(task)
            task.activate()
        # FIXME else if display_on:
        #    print "oops, couldn't find 'preflight' task"

    def request_task_calibrate(self):
        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.name == "calibrate":
                return
        task = self.find_standby_task("calibrate")
        if task:
            # activate task
            self.push_seq_task(task)
            task.activate()
        else:
            # FIXME else if display_on:
            print("oops, couldn't find 'calibrate' task")

    def request_task_land(self, final_heading_deg):
        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.name == "land":
                return
        task = self.find_standby_task( "land" )
        if not task:
            # FIXME if display_on:
            #     print "oops, couldn't find 'land' task"
            return
        # push landing task onto the todo list (and activate)
        self.home_node.setFloat( "azimuth_deg", final_heading_deg )
        self.push_seq_task(task)
        task.activate()

    def request_task_route(self):
        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.name == "route":
                return
        task = self.find_standby_task( "route" )
        if task:
            # activate task
            self.push_seq_task(task)
            task.activate()
        # FIXME else if display_on:
        #    print "oops, couldn't find 'route' task"

    def request_task_parametric(self):
        # sanity check, are we already in the requested state
        if len(self.seq_tasks):
            task = self.seq_tasks[0]
            if task.name == "parametric":
                return
        task = self.find_standby_task( "parametric" )
        if task:
            # activate task
            self.push_seq_task(task)
            task.activate()
            
m = MissionMgr()

def init():
    m.init()

def update(dt):
    m.update(dt)
