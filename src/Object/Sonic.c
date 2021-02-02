#include "Sonic.h"

#include "Object.h"
#include "Game.h"
#include "Level.h"
#include "LevelScroll.h"
#include "LevelCollision.h"
#include "MathUtil.h"

#include <string.h>

//Sonic constants
#define SONIC_WIDTH       9
#define SONIC_HEIGHT      19
#define SONIC_BALL_WIDTH  7
#define SONIC_BALL_HEIGHT 14
#define SONIC_BALL_SHIFT  5

//Sonic mappings
static const uint8_t map_sonic[] = {
	#include <Resource/Mappings/Sonic.h>
};

//Sonic globals
int16_t sonspeed_max, sonspeed_acc, sonspeed_dec;

uint8_t sonframe_num, sonframe_chg;
uint8_t sgfx_buffer[SONIC_DPLC_SIZE];

int16_t track_sonic[0x40][2];
word_u track_pos;

//General Sonic state stuff
static void Sonic_Display(Object *obj)
{
	Scratch_Sonic *scratch = (Scratch_Sonic*)&obj->scratch;
	
	//Handle invulnerability blinking
	uint16_t blink;
	if ((blink = scratch->flash_time))
	{
		scratch->flash_time--;
		if (blink & 4)
			DisplaySprite(obj);
	}
	else
	{
		DisplaySprite(obj);
	}
	
	//Handle invincibility timer
	if (scratch->invincibility_time)
	{
		if (--scratch->invincibility_time == 0)
		{
			//Restore music
			if (!(lock_screen || air < 12))
			{
				//TODO
			}
			
			//Clear flag
			invincibility = false;
		}
	}
	
	//Handle speed shoes timer
	if (scratch->shoes_time)
	{
		if (--scratch->shoes_time == 0)
		{
			//Restore Sonic's speed
			sonspeed_max = 0x600; //BUG: Water isn't checked
			sonspeed_acc = 0xC;
			sonspeed_dec = 0x80;
			
			//Clear flag and restore music
			shoes = false;
			//music	bgm_Slowdown,1,0,0	; run music at normal speed //TODO
		}
	}
}

static void Sonic_RecordPosition(Object *obj)
{
	//Track current position
	int16_t *write = &track_sonic[0][0] + (track_pos.v >> 1);
	*write++ = obj->pos.l.x.f.u;
	*write++ = obj->pos.l.y.f.u;
	track_pos.f.l += 4;
}

//Sonic animation
static const uint8_t anim_sonic[] = {
	#include <Resource/Animation/Sonic.h>
};
#define GET_SONIC_ANISCR(x) (anim_sonic + ((anim_sonic[((x) << 1)] << 8) | (anim_sonic[((x) << 1) + 1] << 0)))

static void Sonic_AnimateReadFrame(Object *obj, const uint8_t *anim_script)
{
	//Read current animation command
	uint8_t cmd = anim_script[1 + obj->anim_frame];
	
	if (!(cmd & 0x80))
	{
		Anim_Next:
		//Set animation frame
		obj->frame = cmd;
		obj->anim_frame++;
	}
	else
	{
		if (++cmd == 0) //0xFF
		{
			//Restart animation
			obj->anim_frame = 0;
			cmd = anim_script[1];
			goto Anim_Next;
		}
		if (++cmd == 0) //0xFE
		{
			//Go back (next byte) frames
			obj->anim_frame -= anim_script[2 + obj->anim_frame];
			cmd = anim_script[1 + obj->anim_frame];
			goto Anim_Next;
		}
		if (++cmd == 0) //0xFD
		{
			//Change animation (falls through to the routine increment below)
			obj->anim = anim_script[2 + obj->anim_frame];
		}
	}
}

static void Sonic_Animate(Object *obj)
{
	//Get animation script to use
	const uint8_t *anim_script = anim_sonic;
	
	//Check if animation changed
	uint8_t anim = obj->anim;
	if (anim != obj->prev_anim)
	{
		//Reset animation state
		obj->prev_anim = anim;
		obj->anim_frame = 0;
		obj->frame_time = 0;
	}
	
	//Get animation script to use
	anim <<= 1;
	anim_script += (anim_script[anim] << 8) | (anim_script[anim + 1] << 0);
	
	int8_t anim_wait = anim_script[0];
	if (anim_wait >= 0)
	{
		//Regular animation
		//Set render flip
		obj->render.f.x_flip = obj->status.p.f.x_flip;
		obj->render.f.y_flip = false;
		
		//Wait for current animation frame to end
		if (--obj->frame_time >= 0)
			return;
		obj->frame_time = anim_wait;
		
		//Read animation
		Sonic_AnimateReadFrame(obj, anim_script);
	}
	else
	{
		//Wait for current animation frame to end
		if (--obj->frame_time >= 0)
			return;
		
		//Special animation (walking, rolling, running, etc.)
		if (++anim_wait == 0)
		{
			//Walking or running
			//Get orienatation
			uint8_t angle = obj->angle;
			uint8_t flip = obj->status.p.f.x_flip;
			if (!flip)
				angle ^= ~0;
			if ((angle += 0x10) & 0x80)
				flip ^= 3;
			
			//Set render flip
			obj->render.f.x_flip = (flip & 1) != 0;
			obj->render.f.y_flip = (flip & 2) != 0;
			
			//Branch to pushing if the pushing flag is set.. but not in the animation
			if (obj->status.p.f.pushing)
				goto Anim_Pushing;
			
			//Get rotation
			angle = (angle >> 4) & 6;
			
			//Get absolute speed
			int16_t abs_spd = obj->inertia;
			if (abs_spd < 0)
				abs_spd = -abs_spd;
			
			//Get script to use
			anim_script = GET_SONIC_ANISCR(SonAnimId_Run);
			if (abs_spd < 0x600)
			{
				anim_script = GET_SONIC_ANISCR(SonAnimId_Walk);
				angle += (angle >> 1);
			}
			angle <<= 1;
			
			//Get animation delay
			int16_t anim_spd = 0x800 - abs_spd;
			if (anim_spd < 0)
				anim_spd = 0;
			obj->frame_time = anim_spd >> 8;
			
			//Read animation
			Sonic_AnimateReadFrame(obj, anim_script);
			obj->frame += angle;
		}
		else if (++anim_wait == 0)
		{
			//Rolling
			//Get absolute speed
			int16_t abs_spd = obj->inertia;
			if (abs_spd < 0)
				abs_spd = -abs_spd;
			
			//Get script to use
			anim_script = GET_SONIC_ANISCR(SonAnimId_Roll2);
			if (abs_spd < 0x600)
				anim_script = GET_SONIC_ANISCR(SonAnimId_Roll);
			
			//Get animation delay
			int16_t anim_spd = 0x400 - abs_spd;
			if (anim_spd < 0)
				anim_spd = 0;
			obj->frame_time = anim_spd >> 8;
			
			//Set render flip
			obj->render.f.x_flip = obj->status.p.f.x_flip;
			obj->render.f.y_flip = false;
			
			//Read animation
			Sonic_AnimateReadFrame(obj, anim_script);
		}
		else
		{
			Anim_Pushing:;
			//Pushing
			//Get absolute speed
			int16_t abs_spd = obj->inertia;
			if (abs_spd < 0)
				abs_spd = -abs_spd;
			
			//Get animation delay
			int16_t anim_spd = 0x800 - abs_spd;
			if (anim_spd < 0)
				anim_spd = 0;
			obj->frame_time = anim_spd >> 6;
			
			//Get script to use
			anim_script = GET_SONIC_ANISCR(SonAnimId_Push);
			
			//Set render flip
			obj->render.f.x_flip = obj->status.p.f.x_flip;
			obj->render.f.y_flip = false;
			
			//Read animation
			Sonic_AnimateReadFrame(obj, anim_script);
		}
	}
}

//Sonic DPLCs
static const uint8_t art_sonic[] = {
	#include <Resource/Art/Sonic.h>
};

static const uint8_t dplc_sonic[] = {
	#include <Resource/Mappings/SonicDPLC.h>
};

static void Sonic_LoadGfx(Object *obj)
{
	//Check if we're loading a new frame
	uint8_t frame = obj->frame;
	if (frame == sonframe_num)
		return;
	sonframe_num = frame;
	
	//Get DPLC script
	const uint8_t *dplc_script = dplc_sonic;
	frame <<= 1;
	dplc_script += (dplc_script[frame] << 8) | (dplc_script[frame + 1] << 0);
	
	//Read number of entries
	int8_t entries = (*dplc_script++) - 1;
	if (entries < 0)
		return;
	
	//Start reading data
	uint8_t *top = sgfx_buffer;
	sonframe_chg = true;
	
	do
	{
		//Read entry
		uint16_t tile = *dplc_script++;
		uint8_t tiles = tile >> 4;
		tile = ((tile << 8) | (*dplc_script++)) << 5;
		
		const uint8_t *fromp = art_sonic + tile;
		do
		{
			memcpy(top, fromp, 0x20);
			fromp += 0x20;
			top += 0x20;
		} while (tiles-- > 0);
	} while (entries-- > 0);
}

//Sonic collision functions
static void Sonic_ResetOnFloor(Object *obj)
{
	Scratch_Sonic *scratch = (Scratch_Sonic*)&obj->scratch;
	
	//Set state
	obj->status.p.f.pushing = false;
	obj->status.p.f.in_air = false;
	obj->status.p.f.roll_jump = false;
	
	if (obj->status.p.f.in_ball)
	{
		obj->status.p.f.in_ball = false;
		obj->y_rad = SONIC_HEIGHT;
		obj->x_rad = SONIC_WIDTH;
		obj->anim = SonAnimId_Walk;
		obj->pos.l.y.f.u -= SONIC_BALL_SHIFT;
	}
	
	scratch->jumping = false;
	item_bonus = 0;
}

static int16_t Sonic_Angle(Object *obj, int16_t dist0, int16_t dist1)
{
	//Get angle and distance to use (use closest one)
	uint8_t res_angle = angle_buffer1;
	int16_t res_dist = dist1;
	if (dist1 >= dist0)
	{
		res_angle = angle_buffer0;
		res_dist = dist0;
	}
	
	//Use adjusted angle if hit angle is odd (special angle, run on all sides)
	if (res_angle & 1)
		obj->angle = (obj->angle + 0x20) & 0xC0;
	else
		obj->angle = res_angle;
	return res_dist;
}

static void Sonic_AnglePos(Object *obj)
{
	Scratch_Sonic *scratch = (Scratch_Sonic*)&obj->scratch;
	
	//Don't do floor collision if standing on an object
	if (obj->status.p.f.object_stand)
	{
		angle_buffer0 = 0;
		angle_buffer1 = 0;
		return;
	}
	
	//Set 'no floor' angle
	angle_buffer0 = 3;
	angle_buffer1 = 3;
	
	//Get symmetrical angle
	uint8_t angle = obj->angle;
	if ((angle + 0x20) & 0x80)
	{
		if (angle & 0x80)
			angle--;
		angle += 0x20;
	}
	else
	{
		if (angle & 0x80)
			angle++;
		angle += 0x1F;
	}
	
	int16_t dist0, dist1, dist;
	switch (angle & 0xC0)
	{
		case 0x00:
			dist0 = FindFloor(obj, obj->pos.l.x.f.u + obj->x_rad, obj->pos.l.y.f.u + obj->y_rad, META_SOLID_TOP, 0, 0x10, &angle_buffer0);
			dist1 = FindFloor(obj, obj->pos.l.x.f.u - obj->x_rad, obj->pos.l.y.f.u + obj->y_rad, META_SOLID_TOP, 0, 0x10, &angle_buffer0);
			if ((dist = Sonic_Angle(obj, dist0, dist1)) != 0)
			{
				if (dist < 0)
				{
					if (dist >= -14)
						obj->pos.l.y.f.u += dist;
				}
				else
				{
					if (dist <= 14)
					{
						obj->pos.l.y.f.u += dist;
					}
					else if (!scratch->x38.floor_clip)
					{
						obj->status.p.f.in_air = true;
						obj->status.p.f.pushing = false;
						obj->prev_anim = 1;
					}
				}
			}
			break;
	}
}

//Sonic functions
int KillSonic(Object *obj, Object *src)
{
	Scratch_Sonic *scratch = (Scratch_Sonic*)&obj->scratch;
	
	//Check if we can be killed
	if (debug_use)
		return -1;
	
	//Set state
	invincibility = false;
	obj->routine = 6;
	Sonic_ResetOnFloor(obj);
	obj->status.p.f.in_air = true;
	obj->ysp = -0x700;
	obj->xsp = 0;
	obj->inertia = 0;
	scratch->x38.death_y = obj->pos.l.y.f.u;
	obj->anim = SonAnimId_Death;
	obj->tile.s.priority = true;
	
	//Play death sound
	//move.w	#sfx_Death,d0	; play normal death sound
	//cmpi.b	#id_Spikes,(a2)	; check	if you were killed by spikes
	//bne.s	@sound
	//move.w	#sfx_HitSpikes,d0 ; play spikes death sound
	//
	//@sound:
	//jsr	(PlaySound_Special).l
	return -1;
}

//Sonic movement functions
static bool Sonic_Jump(Object *obj)
{
	Scratch_Sonic *scratch = (Scratch_Sonic*)&obj->scratch;
	
	//Don't jump if ABC isn't pressed
	if (!(jpad1_press2 & (JPAD_A | JPAD_C | JPAD_B)))
		return false;
	
	//Check if we have enough room to jump
	if (GetDistanceBelowAngle(obj, obj->angle + 0x80, NULL) < 6)
		return false;
	
	//Get jump speed
	int16_t speed = 0x680;
	if (obj->status.p.f.underwater)
		speed = 0x380;
	
	int16_t sin, cos;
	CalcSine(obj->angle - 0x40, &sin, &cos);
	obj->xsp += (cos * speed) >> 8;
	obj->ysp += (sin * speed) >> 8;
	
	//Set state
	obj->status.p.f.in_air = true;
	obj->status.p.f.pushing = false;
	scratch->jumping = true;
	scratch->x38.floor_clip = 0;
	
	//sfx	sfx_Jump,0,0,0	; play jumping sound //TODO
	
	obj->y_rad = SONIC_HEIGHT; //No idea why this is here
	obj->x_rad = SONIC_WIDTH;
	
	if (!obj->status.p.f.in_ball)
	{
		obj->y_rad = SONIC_BALL_HEIGHT;
		obj->x_rad = SONIC_BALL_WIDTH;
		obj->anim = SonAnimId_Roll;
		obj->status.p.f.in_ball = true;
		obj->pos.l.y.f.u += SONIC_BALL_SHIFT;
	}
	
	return true;
}

static void Sonic_SlopeResist(Object *obj)
{
	if (((obj->angle + 0x20) & 0xC0) >= 0x60)
		return;
	
	int16_t force = (GetSin(obj->angle) * 0x20) >> 8;
	if (obj->inertia != 0)
		obj->inertia += force;
}

static void Sonic_MoveLeft(Object *obj)
{
	int16_t inertia = obj->inertia;
	if (inertia <= 0)
	{
		//Turn around
		if (!obj->status.p.f.x_flip)
		{
			obj->status.p.f.x_flip = true;
			obj->status.p.f.pushing = false;
			obj->prev_anim = 1; //Reset animation
		}
		
		//Accelerate
		if ((inertia -= sonspeed_acc) <= -sonspeed_max)
			inertia = -sonspeed_max;
		
		//Set speed and animation
		obj->inertia = inertia;
		obj->anim = SonAnimId_Walk;
	}
	else
	{
		//Decelerate
		if ((inertia -= sonspeed_dec) < 0)
			inertia = -0x80;
		obj->inertia = inertia;
		
		//Skid
		if (((obj->angle + 0x20) & 0xC0) == 0x00 && inertia >= 0x400)
		{
			obj->anim = SonAnimId_Stop;
			obj->status.p.f.x_flip = false;
			//sfx	sfx_Skid,0,0,0	; play stopping sound //TODO
		}
	}
}

static void Sonic_MoveRight(Object *obj)
{
	int16_t inertia = obj->inertia;
	if (inertia >= 0)
	{
		//Turn around
		if (obj->status.p.f.x_flip)
		{
			obj->status.p.f.x_flip = false;
			obj->status.p.f.pushing = false;
			obj->prev_anim = 1; //Reset animation
		}
		
		//Accelerate
		if ((inertia += sonspeed_acc) >= sonspeed_max)
			inertia = sonspeed_max;
		
		//Set speed and animation
		obj->inertia = inertia;
		obj->anim = SonAnimId_Walk;
	}
	else
	{
		//Decelerate
		if ((inertia += sonspeed_dec) >= 0)
			inertia = 0x80;
		obj->inertia = inertia;
		
		//Skid
		if (((obj->angle + 0x20) & 0xC0) == 0x00 && inertia <= -0x400)
		{
			obj->anim = SonAnimId_Stop;
			obj->status.p.f.x_flip = true;
			//sfx	sfx_Skid,0,0,0	; play stopping sound //TODO
		}
	}
}

static void Sonic_Move(Object *obj)
{
	Scratch_Sonic *scratch = (Scratch_Sonic*)&obj->scratch;
	
	if (!jump_only)
	{
		if (!scratch->control_lock)
		{
			//Move left and right according to held direction
			if (jpad1_hold2 & JPAD_LEFT)
				Sonic_MoveLeft(obj);
			if (jpad1_hold2 & JPAD_RIGHT)
				Sonic_MoveRight(obj);
			
			//Do idle or balance animation
			if (((obj->angle + 0x20) & 0xC0) == 0x00 && !obj->inertia)
			{
				//Do idle animation
				obj->status.p.f.pushing = false;
				obj->anim = SonAnimId_Wait;
				
				if (obj->status.p.f.object_stand)
				{
					//Balance on an object
					Object *stand = &objects[scratch->standing_obj];
					if (!stand->status.o.f.flag7)
					{
						int16_t left_dist = stand->width_pixels;
						int16_t right_dist = left_dist + left_dist - 4;
						left_dist += obj->pos.l.x.f.u - stand->pos.l.x.f.u;
						
						if (left_dist < 4)
							obj->status.p.f.x_flip = true;
						else if (left_dist >= right_dist)
							obj->status.p.f.x_flip = false;
						obj->anim = SonAnimId_Balance;
						goto Sonic_ResetScr;
					}
				}
				else
				{
					//Balance on level
					if (ObjFloorDist(obj, obj->pos.l.x.f.u) >= 12)
					{
						if (scratch->front_angle == 3)
						{
							obj->status.p.f.x_flip = false;
							obj->anim = SonAnimId_Balance;
							goto Sonic_ResetScr;
						}
						if (scratch->back_angle == 3)
						{
							obj->status.p.f.x_flip = true;
							obj->anim = SonAnimId_Balance;
							goto Sonic_ResetScr;
						}
					}
				}
				
				//Handle looking up and down
				if (jpad1_hold2 & JPAD_UP)
				{
					obj->anim = SonAnimId_LookUp;
					if (look_shift != (200 + SCREEN_TALLADD2))
						look_shift += 2;
					goto loc_12FC2;
				}
				if (jpad1_hold2 & JPAD_DOWN)
				{
					obj->anim = SonAnimId_Duck;
					if (look_shift != (8 + SCREEN_TALLADD2))
						look_shift -= 2;
					goto loc_12FC2;
				}
			}
		}
		
		//Reset camera to neutral position
		Sonic_ResetScr:;
		if (look_shift < (192 + SCREEN_TALLADD2))
			look_shift += 2;
		else if (look_shift > (192 + SCREEN_TALLADD2))
			look_shift -= 2;
		loc_12FC2:;
		
		//Friction
		if (!(jpad1_hold2 & (JPAD_LEFT | JPAD_RIGHT)))
		{
			if (obj->inertia > 0)
			{
				if ((obj->inertia -= sonspeed_acc) < 0)
					obj->inertia = 0;
			}
			else
			{
				if ((obj->inertia += sonspeed_acc) >= 0)
					obj->inertia = 0;
			}
		}
	}
	
	//Calculate global speed from inertia
	int16_t sin, cos;
	CalcSine(obj->angle, &sin, &cos);
	obj->xsp = (cos * obj->inertia) >> 8;
	obj->ysp = (sin * obj->inertia) >> 8;
	
	//Handle wall collision
	if (((obj->angle + 0x40) & 0x80) || !obj->inertia)
		return;
	
	uint8_t add_angle = (obj->inertia < 0) ? 0x40 : -0x40;
	uint8_t angle = obj->angle + add_angle;
	int16_t dist = GetDistanceBelowAngle2(obj, angle, NULL);
	
	if (dist < 0)
	{
		dist <<= 8;
		switch ((angle + 0x20) & 0xC0)
		{
			case 0x00:
				obj->ysp += dist;
				break;
			case 0x40:
				obj->xsp -= dist;
				obj->status.p.f.pushing = true;
				obj->inertia = 0;
				break;
			case 0x80:
				obj->ysp -= dist;
				break;
			case 0xC0:
				obj->xsp += dist;
				obj->status.p.f.pushing = true;
				obj->inertia = 0;
				break;
		}
	}
}

void Sonic_ChkRoll(Object *obj)
{
	//Enter roll state
	if (obj->status.p.f.in_ball)
		return;
	obj->status.p.f.in_ball = true;
	obj->y_rad = SONIC_BALL_HEIGHT;
	obj->x_rad = SONIC_BALL_WIDTH;
	obj->anim = SonAnimId_Roll;
	obj->pos.l.y.f.u += SONIC_BALL_SHIFT;
	//sfx	sfx_Roll,0,0,0	; play rolling sound //TODO
	
	//Set speed (S-tubes)
	if (!obj->inertia)
		obj->inertia = 0x200;
}

static void Sonic_Roll(Object *obj)
{
	//Check if we can and are trying to roll
	if (jump_only || ((obj->inertia < 0) ? -obj->inertia : obj->inertia) < 0x80)
		return;
	if ((jpad1_hold2 & (JPAD_LEFT | JPAD_RIGHT)) || !(jpad1_hold2 & JPAD_DOWN))
		return;
	Sonic_ChkRoll(obj);
}

static void Sonic_LevelBound(Object *obj)
{
	//Get next X position
	//This is unsigned, but it shouldn't be
	uint16_t x = (obj->pos.l.x.v + (obj->xsp << 8)) >> 16;
	
	//Prevent us from going off the left or right boundaries
	int16_t bound;
	if (x < (bound = limit_left2 + 16) || x > (bound = limit_right2 + (lock_screen ? 290 : 360) + SCREEN_WIDEADD))
	{
		obj->pos.l.x.f.u = bound;
		obj->pos.l.x.f.l = 0;
		obj->xsp = 0;
		obj->inertia = 0;
	} 
	
	//Fall off the bottom boundary
	if (obj->pos.l.y.f.u >= limit_btm2 + SCREEN_HEIGHT)
	{
		if (level_id == LEVEL_ID(ZoneId_SBZ, 1) && obj->pos.l.x.f.u >= 0x2000)
		{
			//Go to SBZ3 if falling off at the end of SBZ2
			last_lamp = 0;
			restart = true;
			level_id = LEVEL_ID(ZoneId_LZ, 3);
		}
		else
		{
			//Kill Sonic
			KillSonic(obj, obj); //The second argument is undefined in the original
		}
	}
}

//Sonic object
void Obj_Sonic(Object *obj)
{
	Scratch_Sonic *scratch = (Scratch_Sonic*)&obj->scratch;
	
	//Run debug mode code while in debug mode
	if (debug_use)
	{
		//DebugMode();
		return;
	}
	
	//Run player routine
	switch (obj->routine)
	{
		case 0: //Initialiation
			//Increment routine
			obj->routine += 2;
			
			//Initialize collision size
			obj->y_rad = SONIC_HEIGHT;
			obj->x_rad = SONIC_WIDTH;
			
			//Set object drawing information
			obj->mappings = map_sonic;
			obj->tile.w = 0;
			obj->tile.s.pattern = 0x780;
			obj->priority = 2;
			obj->width_pixels = 24;
			obj->render.b = 0;
			obj->render.f.align_fg = true;
			
			//Initialize speeds
			sonspeed_max = 0x600;
			sonspeed_acc = 0xC;
			sonspeed_dec = 0x80;
	//Fallthrough
		case 2:
			//Enter debug mode
			if (debug_cheat && (jpad1_press1 & JPAD_B))
			{
				debug_use = true;
				lock_ctrl = false;
				break;
			}
			
			//Copy player controls if not locked
			if (!lock_ctrl)
			{
				jpad1_hold2  = jpad1_hold1;
				jpad1_press2 = jpad1_press1;
			}
			
			//Run player routine
			if (!(lock_multi & 1))
			{
				switch ((obj->status.p.f.in_ball << 2) | (obj->status.p.f.in_air << 1))
				{
					case 0: //Not in ball, not in air
						if (Sonic_Jump(obj))
							break;
						Sonic_SlopeResist(obj);
						Sonic_Move(obj);
						Sonic_Roll(obj);
						Sonic_LevelBound(obj);
						SpeedToPos(obj);
						Sonic_AnglePos(obj);
						break;
					case 2: //Not in ball, in air
						Sonic_LevelBound(obj);
						ObjectFall(obj);
						if (obj->status.p.f.underwater)
							obj->ysp -= 0x28;
						break;
					case 4: //In ball, not in air
						break;
					case 6: //In ball, in air
						Sonic_LevelBound(obj);
						ObjectFall(obj);
						if (obj->status.p.f.underwater)
							obj->ysp -= 0x28;
						break;
				}
			}
			
			//Handle general player state stuff
			Sonic_Display(obj);
			Sonic_RecordPosition(obj);
			//Sonic_Water(obj); //TODO
			
			//Copy angle buffers
			scratch->front_angle = angle_buffer0;
			scratch->back_angle  = angle_buffer1;
			
			//Animate
			if (tunnel_mode)
			{
				if (!obj->anim)
					obj->prev_anim = obj->anim;
			}
			Sonic_Animate(obj);
			
			//Handle DPLCs
			Sonic_LoadGfx(obj);
			break;
	}
}