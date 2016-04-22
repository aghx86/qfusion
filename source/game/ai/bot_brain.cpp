#include "bot.h"
#include "bot_brain.h"
#include "../../gameshared/q_comref.h"
#include <algorithm>
#include <limits>
#include <stdarg.h>

constexpr float MAX_ENEMY_WEIGHT = 5.0f;

inline const char *Nick(const edict_t *ent)
{
    return ent && ent->r.client ? ent->r.client->netname : ent->classname;
}

inline const char *WeapName(int weapon)
{
    return GS_GetWeaponDef(weapon)->name;
}

float DamageToKill(const edict_t *client, float armorProtection, float armorDegradation)
{
    if (!client || !client->r.client)
        return 0.0f;

    float health = client->r.client->ps.stats[STAT_HEALTH];
    float armor = client->r.client->ps.stats[STAT_ARMOR];

    if (!armor)
        return health;
    if (armorProtection == 1.0f)
        return std::numeric_limits<float>::infinity();

    if (armorDegradation != 0)
    {
        float damageToWipeArmor = armor / armorDegradation;
        float healthDamageToWipeArmor = damageToWipeArmor * (1.0f - armorProtection);

        if (healthDamageToWipeArmor < health)
            return damageToWipeArmor + (health - healthDamageToWipeArmor);

        return health / (1.0f - armorProtection);
    }

    return health / (1.0f - armorProtection);
}

void Enemy::Clear()
{
    ent = nullptr;
    weight = 0.0f;
    avgPositiveWeight = 0.0f;
    maxPositiveWeight = 0.0f;
    positiveWeightsCount = 0;
    registeredAt = 0;
    lastSeenPositions.clear();
    lastSeenTimestamps.clear();
    lastSeenVelocities.clear();
    lastSeenAt = 0;
}

void Enemy::OnViewed()
{
    if (lastSeenPositions.size() == MAX_TRACKED_POSITIONS)
    {
        lastSeenPositions.pop_back();
        lastSeenTimestamps.pop_back();
        lastSeenVelocities.pop_back();
    }
    // Set members for faster access
    VectorCopy(ent->s.origin, lastSeenPosition.data());
    VectorCopy(ent->velocity, lastSeenVelocity.data());
    lastSeenAt = level.time;
    // Store in a queue then for history
    lastSeenPositions.push_front(lastSeenPosition);
    lastSeenVelocities.push_front(lastSeenVelocity);
    lastSeenTimestamps.push_front(lastSeenAt);
}

BotBrain::BotBrain(edict_t *bot, float skillLevel)
    : AiBaseBrain(bot, TFL_DEFAULT, TFL_DEFAULT),
      bot(bot),
      skillLevel(skillLevel),
      trackedEnemiesCount(0),
      maxTrackedEnemies(3 + (unsigned)((MAX_TRACKED_ENEMIES-3) * BotSkill())),
      maxTrackedAttackers(1 + (unsigned)((MAX_TRACKED_ATTACKERS-1) * BotSkill())),
      maxTrackedTargets(1 + (unsigned)((MAX_TRACKED_TARGETS-1) * BotSkill())),
      reactionTime(32 + (unsigned)(300.0f * (1.0f - BotSkill()))),
      aimTargetChoicePeriod(1000 - (unsigned)(900.0f * BotSkill())),
      spamTargetChoicePeriod(1333 - (unsigned)(500.0f * BotSkill())),
      aimWeaponChoicePeriod(1032 - (unsigned)(500.0f * BotSkill())),
      spamWeaponChoicePeriod(1000 - (unsigned)(333.0f * BotSkill())),
      combatTaskInstanceCounter(1),
      nextTargetChoiceAt(level.time),
      nextWeaponChoiceAt(level.time),
      nextFastWeaponSwitchActionCheckAt(level.time),
      prevThinkLevelTime(level.time),
      armorProtection(g_armor_protection->value),
      armorDegradation(g_armor_degradation->value),
      weaponScoreRandom(0.0f),
      nextWeaponScoreRandomUpdate(level.time),
      decisionRandom(0.5f),
      nextDecisionRandomUpdate(level.time)
{
#ifdef _DEBUG
    float skill = BotSkill();
    unsigned maxEnemies = maxTrackedEnemies;
    // Ensure we always will have at least 2 free slots for new enemies
    // (quad/shell owners and carrier) FOR ANY SKILL (high skills will have at least 3)
    if (maxTrackedAttackers + 2 > maxEnemies)
    {
        printf("skill %f: maxTrackedAttackers %d + 2 > maxTrackedEnemies %d\n", skill, maxTrackedAttackers, maxEnemies);
        abort();
    }
    if (maxTrackedTargets + 2 > maxEnemies)
    {
        printf("skill %f: maxTrackedTargets %d + 2 > maxTrackedEnemies %d\n", skill, maxTrackedTargets, maxEnemies);
        abort();
    }
#endif

    // Initialize empty slots
    for (unsigned i = 0; i < maxTrackedEnemies; ++i)
        enemies.push_back(Enemy());

    for (unsigned i = 0; i < maxTrackedAttackers; ++i)
        attackers.emplace_back(AttackStats());

    for (unsigned i = 0; i < maxTrackedTargets; ++i)
        targets.emplace_back(AttackStats());
}

void BotBrain::PreThink()
{
    const unsigned levelTime = level.time;
    for (Enemy &enemy: enemies)
    {
        // If enemy slot is free
        if (!enemy.ent)
        {
            continue;
        }
        // Remove not seen yet enemies
        if (levelTime - enemy.LastSeenAt() > NOT_SEEN_TIMEOUT)
        {
            Debug("has not seen %s for %d ms, should forget this enemy\n", enemy.Nick(), NOT_SEEN_TIMEOUT);
            RemoveEnemy(enemy);
            continue;
        }
        if (G_ISGHOSTING(enemy.ent))
        {
            Debug("should forget %s (this enemy is ghosting)\n", enemy.Nick());
            RemoveEnemy(enemy);
            continue;
        }
        // Do not forget, just skip
        if (enemy.ent->flags & (FL_NOTARGET|FL_BUSY))
            continue;
        // Skip during reaction time
        if (enemy.registeredAt + reactionTime > levelTime)
            continue;

        UpdateWeight(enemy);
    }

    if (combatTask.spamEnemy)
    {
        if (combatTask.spamTimesOutAt <= levelTime)
        {
            Debug("spamming at %s has timed out\n", combatTask.spamEnemy->Nick());
            combatTask.Reset();
            nextTargetChoiceAt = levelTime;
        }
    }

    if (nextWeaponScoreRandomUpdate <= levelTime)
    {
        weaponScoreRandom = brandom(0, 1.0f - BotSkill()) - 0.5f;
        nextWeaponScoreRandomUpdate = levelTime + 4000;
    }
    if (nextDecisionRandomUpdate <= levelTime)
    {
        decisionRandom = random();
        nextDecisionRandomUpdate = levelTime + 2500;
    }
}

void BotBrain::PostThink()
{
    prevThinkLevelTime = level.time;
}

void BotBrain::Frame()
{
    // Call superclass method first
    AiBaseBrain::Frame();

    for (AttackStats &attackerStats: attackers)
    {
        attackerStats.Frame();
        if (attackerStats.LastActivityAt() + ATTACKER_TIMEOUT < level.time)
            attackerStats.Clear();
    }

    for (AttackStats &targetStats: targets)
    {
        targetStats.Frame();
        if (targetStats.LastActivityAt() + TARGET_TIMEOUT < level.time)
            targetStats.Clear();
    }
}

void BotBrain::Think()
{
    // Call superclass method first
    AiBaseBrain::Think();

    if (nextFastWeaponSwitchActionCheckAt <= level.time)
    {
        if (CheckFastWeaponSwitchAction())
        {
            nextFastWeaponSwitchActionCheckAt = level.time + 500;
            if (nextTargetChoiceAt <= level.time + 64)
                nextTargetChoiceAt += 64 + 1;
            if (nextWeaponChoiceAt <= level.time + 64)
                nextWeaponChoiceAt += 64 + 1;
            return;
        }
    }

    if (combatTask.aimEnemy && (level.time - combatTask.aimEnemy->LastSeenAt()) > reactionTime)
    {
        TryFindNewCombatTask();
    }
    else if (nextTargetChoiceAt > level.time)
    {
        UpdateKeptCurrentCombatTask();
    }
    else
    {
        TryFindNewCombatTask();
    }
}

void BotBrain::UpdateWeight(Enemy &enemy)
{
    if (level.time - enemy.LastSeenAt() > reactionTime)
    {
        enemy.weight = 0;
        return;
    }

    enemy.weight = ComputeRawWeight(enemy.ent);
    if (enemy.weight > enemy.maxPositiveWeight)
    {
        enemy.maxPositiveWeight = enemy.weight;
        //AiDebug("%s UpdateWeight(%s): maxPositiveWeight: %.2f\n", BotNick(), enemy.Nick(), enemy.maxPositiveWeight);
    }
    if (enemy.weight > 0)
    {
        enemy.avgPositiveWeight = enemy.avgPositiveWeight * enemy.positiveWeightsCount + enemy.weight;
        enemy.positiveWeightsCount++;
        enemy.avgPositiveWeight /= enemy.positiveWeightsCount;
        //AiDebug("%s UpdateWeight(%s): avgPositiveWeight: %.2f\n", BotNick(), enemy.Nick(), enemy.avgPositiveWeight);
    }
}

void BotBrain::RemoveEnemy(Enemy &enemy)
{
    // Enemies always are located in the same buffer, so we may compare pointers
    if (&enemy == combatTask.aimEnemy)
    {
        combatTask.Reset();
        combatTask.prevSpamEnemy = nullptr;
        nextTargetChoiceAt = level.time + reactionTime;
    }
    else if (&enemy == combatTask.spamEnemy)
    {
        combatTask.Reset();
        combatTask.prevSpamEnemy = nullptr;
        nextTargetChoiceAt = level.time + reactionTime;
    }
    enemy.Clear();
    --trackedEnemiesCount;
}

bool BotBrain::HasAnyDetectedEnemiesInView() const
{
    for (const Enemy &enemy: enemies)
    {
        if (!enemy.ent)
            continue;
        if (enemy.LastSeenAt() == level.time)
        {
            // Check whether we may react
            for (unsigned seenTimestamp: enemy.lastSeenTimestamps)
            {
                if (seenTimestamp + reactionTime <= level.time)
                    return true;
            }
        }
    }
    return false;
}

void BotBrain::AfterAllEnemiesViewed()
{
    CheckIsInThinkFrame(__FUNCTION__);

    // Stop spamming if we see any enemy in view, choose a target to fight
    if (combatTask.spamEnemy)
    {
        if (HasAnyDetectedEnemiesInView())
        {
            Debug("should stop spamming at %s, there are enemies in view\n", combatTask.spamEnemy->Nick());
            combatTask.Reset();
            nextTargetChoiceAt = level.time;
        }
    }
}

void BotBrain::OnEnemyViewed(const edict_t *enemy)
{
    CheckIsInThinkFrame(__FUNCTION__);

    if (!enemy)
        return;

    int freeSlot = -1;
    for (unsigned i = 0; i < enemies.size(); ++i)
    {
        // Use first free slot for faster access and to avoid confusion
        if (!enemies[i].ent && freeSlot < 0)
        {
            freeSlot = i;
        }
        else if (enemies[i].ent == enemy)
        {
            enemies[i].OnViewed();
            return;
        }
    }

    if (freeSlot >= 0)
    {
        Debug("has viewed a new enemy %s, uses free slot #%d to remember it\n", Nick(enemy), freeSlot);
        EmplaceEnemy(enemy, freeSlot);
        trackedEnemiesCount++;
    }
    else
    {
        Debug("has viewed a new enemy %s, all slots are used. Should try evict some slot\n", Nick(enemy));
        TryPushNewEnemy(enemy);
    }
}

void BotBrain::EmplaceEnemy(const edict_t *enemy, int slot)
{
    Enemy &slotEnemy = enemies[slot];
    slotEnemy.ent = enemy;
    slotEnemy.registeredAt = level.time;
    slotEnemy.weight = 0.0f;
    slotEnemy.avgPositiveWeight = 0.0f;
    slotEnemy.maxPositiveWeight = 0.0f;
    slotEnemy.positiveWeightsCount = 0;
    slotEnemy.OnViewed();
    Debug("has stored enemy %s in slot %d\n", slotEnemy.Nick(), slot);
}

void BotBrain::TryPushNewEnemy(const edict_t *enemy)
{
    // Try to find a free slot. For each used and not reserved slot compute eviction score relative to new enemy

    int candidateSlot = -1;

    // Floating point computations for zero cases from pure math point of view may yield a non-zero result,
    // so use some positive value that is greater that possible computation zero epsilon.
    float maxEvictionScore = 0.001f;
    // Significantly increases chances to get a slot, but not guarantees it.
    bool isNewEnemyAttacker = LastAttackedByTime(enemy) > 0;
    // It will be useful inside the loop, so it needs to be precomputed
    float distanceToNewEnemy = (Vec3(bot->s.origin) - enemy->s.origin).LengthFast();
    float newEnemyWeight = ComputeRawWeight(enemy);

    for (unsigned i = 0; i < maxTrackedEnemies; ++i)
    {
        Enemy &slotEnemy = enemies[i];
        // Skip last attackers
        if (LastAttackedByTime(slotEnemy.ent) > 0)
            continue;
        // Skip last targets
        if (LastTargetTime(slotEnemy.ent) > 0)
            continue;

        // Never evict powerup owners or item carriers
        if (slotEnemy.HasPowerups() || slotEnemy.IsCarrier())
            continue;

        float currEvictionScore = 0.0f;
        if (isNewEnemyAttacker)
            currEvictionScore += 0.5f;

        float absWeightDiff = slotEnemy.weight - newEnemyWeight;
        if (newEnemyWeight > slotEnemy.weight)
        {
            currEvictionScore += newEnemyWeight - slotEnemy.weight;
        }
        else
        {
            if (BotSkill() < 0.66f)
            {
                if (decisionRandom > BotSkill())
                    currEvictionScore += (1.0 - BotSkill()) * expf(-absWeightDiff);
            }
        }

        // Forget far and not seen enemies
        if (slotEnemy.LastSeenAt() < prevThinkLevelTime)
        {
            float absTimeDiff = prevThinkLevelTime - slotEnemy.LastSeenAt();
            // 0..1
            float timeFactor = std::min(absTimeDiff, (float)NOT_SEEN_TIMEOUT) / NOT_SEEN_TIMEOUT;

            float distanceToSlotEnemy = (slotEnemy.LastSeenPosition() - bot->s.origin).LengthFast();
            constexpr float maxDistanceDiff = 2500.0f;
            float nonNegDistDiff = std::max(0.0f, distanceToSlotEnemy - distanceToNewEnemy);
            // 0..1
            float distanceFactor = std::min(maxDistanceDiff, nonNegDistDiff) / maxDistanceDiff;

            // += 0..1,  Increase eviction score linearly for far enemies
            currEvictionScore += 1.0f - distanceFactor;
            // += 2..0, Increase eviction score non-linearly for non-seen enemies (forget far enemies faster)
            currEvictionScore += 2.0f - timeFactor * (1.0f + distanceFactor);
        }

        if (currEvictionScore > maxEvictionScore)
        {
            maxEvictionScore = currEvictionScore;
            candidateSlot = i;
        }
    }

    if (candidateSlot != -1)
    {
        Debug("will evict %s to make a free slot, new enemy have higher priority atm\n", Nick(enemy));
        EmplaceEnemy(enemy, candidateSlot);
    }
    else
    {
        Debug("can't find free slot for %s, all current enemies have higher priority\n", Nick(enemy));
    }
}

float BotBrain::ComputeRawWeight(const edict_t *enemy)
{
    if (!enemy || G_ISGHOSTING(enemy))
        return 0.0;

    float weight = 0.5f;

    if (unsigned time = LastAttackedByTime(enemy))
    {
        weight += 0.75f * ((level.time - time) / (float) ATTACKER_TIMEOUT);
        // TODO: Add weight for poor attackers (by total damage / attack attepts ratio)
    }

    if (unsigned time = LastTargetTime(enemy))
    {
        weight += 1.55f * ((level.time - time) / (float) TARGET_TIMEOUT);
        // TODO: Add weight for targets that are well hit by bot
    }

    if (::IsCarrier(enemy))
        weight += 2.0f;

    constexpr float maxDamageToKill = 350.0f;

    const bool hasQuad = BotHasQuad();
    const bool hasShell = BotHasShell();

    float damageToKill = DamageToKill(enemy);
    if (hasQuad && !HasShell(enemy))
        damageToKill /= 4;

    float damageToBeKilled = DamageToKill(bot);
    if (hasShell && !HasQuad(enemy))
        damageToBeKilled /= 4;

    // abs(damageToBeKilled - damageToKill) / maxDamageToKill may be > 1
    weight += (damageToBeKilled - damageToKill) / maxDamageToKill;

    if (weight > 0)
    {
        if (hasQuad)
            weight *= 1.5f;
        if (hasShell)
            weight += 0.5f;
        if (hasQuad && hasShell)
            weight *= 1.5f;
    }

    float result = std::min(std::max(0.0f, weight), MAX_ENEMY_WEIGHT);
    //AiDebug("%s has computed a weight for %s, the weight is %f\n", BotNick(), Nick(enemy), result);
    return result;
}

unsigned BotBrain::LastAttackedByTime(const edict_t *ent) const
{
    for (const AttackStats &attackStats: attackers)
        if (ent && attackStats.ent == ent)
            return attackStats.LastActivityAt();

    return 0;
}

unsigned BotBrain::LastTargetTime(const edict_t *ent) const
{
    for (const AttackStats &targetStats: targets)
        if (ent && targetStats.ent == ent)
            return targetStats.LastActivityAt();

    return 0;
}

void BotBrain::OnPain(const edict_t *enemy, float kick, int damage)
{
    EnqueueAttacker(enemy, damage);
}

void BotBrain::EnqueueAttacker(const edict_t *attacker, int damage)
{
    if (!attacker)
        return;

    int freeSlot = -1;
    for (unsigned i = 0; i < attackers.size(); ++i)
    {
        if (attackers[i].ent == attacker)
        {
            attackers[i].OnDamage(damage);
            return;
        }
        else if (!attackers[i].ent && freeSlot < 0)
            freeSlot = i;
    }
    if (freeSlot >= 0)
    {
        attackers[freeSlot].ent = attacker;
        attackers[freeSlot].OnDamage(damage);
        return;
    }
    float maxEvictionScore = 0.0f;
    for (unsigned i = 0; i < attackers.size(); ++i)
    {
        float timeFactor = (level.time - attackers[i].LastActivityAt()) / (float)ATTACKER_TIMEOUT;
        float damageFactor = 1.0f - BoundedFraction(attackers[i].totalDamage, 500.0f);
        // Always > 0, so we always evict some attacker
        float evictionScore = 0.1f + timeFactor * damageFactor;
        if (maxEvictionScore < evictionScore)
        {
            maxEvictionScore = evictionScore;
            freeSlot = i;
        }
    }
    attackers[freeSlot].ent = attacker;
    attackers[freeSlot].OnDamage(damage);
}

void BotBrain::EnqueueTarget(const edict_t *target)
{
    if (!target)
        return;

    int freeSlot = -1;
    for (unsigned i = 0; i < targets.size(); ++i)
    {
        if (targets[i].ent == target)
        {
            targets[i].Touch();
            return;
        }
        else if (!targets[i].ent && freeSlot < 0)
            freeSlot = i;
    }
    if (freeSlot >= 0)
    {
        targets[freeSlot].ent = target;
        targets[freeSlot].Touch();
        return;
    }
    float maxEvictionScore = 0.0f;
    for (unsigned i = 0; i < targets.size(); ++i)
    {
        float timeFactor = (level.time - targets[i].LastActivityAt()) / (float)TARGET_TIMEOUT;
        // Do not evict enemies that bot hit hard
        float damageScale = BotHasQuad() ? 4.0f : 1.0f;
        float damageFactor = 1.0f - BoundedFraction(targets[i].totalDamage, 300.0f * damageScale);
        // Always > 0, so we always evict some target
        float evictionScore = 0.1f + timeFactor * damageFactor;
        if (maxEvictionScore < evictionScore)
        {
            maxEvictionScore = evictionScore;
            freeSlot = i;
        }
    }
    targets[freeSlot].ent = target;
    targets[freeSlot].Touch();
}

void BotBrain::OnEnemyDamaged(const edict_t *target, int damage)
{
    if (!target)
        return;
    for (unsigned i = 0; i < targets.size(); ++i)
    {
        if (targets[i].ent == target)
        {
            targets[i].OnDamage(damage);
            return;
        }
    }
}

void BotBrain::UpdateKeptCurrentCombatTask()
{
    auto *task = &combatTask;
    if (nextWeaponChoiceAt <= level.time)
    {
        if (task->aimEnemy)
        {
            nextWeaponChoiceAt = level.time + aimWeaponChoicePeriod;
            SuggestAimWeaponAndTactics(task);
            Debug("UpdateKeptCombatTask(): has aim enemy, next weapon choice at %09d\n", nextWeaponChoiceAt);
        }
        else if (task->spamEnemy)
        {
            nextWeaponChoiceAt = level.time + spamWeaponChoicePeriod;
            SuggestSpamEnemyWeaponAndTactics(task);
            Debug("UpdateKeptCombatTask(): has spam enemy, next weapon choice at %09d\n", nextWeaponChoiceAt);
        }
    }
}

bool BotBrain::CheckFastWeaponSwitchAction()
{
    if (!combatTask.aimEnemy)
        return false;

    if (self->r.client->ps.stats[STAT_WEAPON_TIME] >= 64)
        return false;

    // Easy bots do not perform fast weapon switch actions
    if (BotSkill() < 0.33f)
        return false;

    const Enemy &enemy = *combatTask.aimEnemy;
    CombatDisposition disposition = GetCombatDisposition(*combatTask.aimEnemy);

    int chosenWeapon = WEAP_NONE;
    if (disposition.damageToKill < 75)
    {
        Debug("decided to finish %s that can resist %.1f damage units\n", enemy.Nick(), disposition.damageToKill);
        chosenWeapon = SuggestFinishWeapon(enemy, disposition);
        Debug("chose %s to finish %s\n", WeapName(chosenWeapon), enemy.Nick());
    }
    // Try to hit escaping enemies hard in a single shot
    else if (IsEnemyEscaping(enemy, disposition))
    {
        Debug("detected that %s is escaping\n", enemy.Nick());
        chosenWeapon = SuggestHitEscapingEnemyWeapon(enemy, disposition);
        Debug("chose %s to hit escaping %s\n", WeapName(chosenWeapon), enemy.Nick());
    }
    // Try to hit enemy hard in a single shot before death
    else if (CheckForShotOfDespair(enemy, disposition))
    {
        Debug("decided to do a shot of despair having %.1f damage units to be killed\n", disposition.damageToBeKilled);
        chosenWeapon = SuggestShotOfDespairWeapon(enemy, disposition);
        Debug("chose %s for shot of despair at %s\n", WeapName(chosenWeapon), enemy.Nick());
    }

    if (chosenWeapon != WEAP_NONE)
    {
        combatTask.suggestedShootWeapon = chosenWeapon;
        combatTask.importantShot = true;
        return true;
    }

    return false;
}

void BotBrain::TryFindNewCombatTask()
{
    CombatTask *task = &combatTask;
    const Enemy *oldAimEnemy = task->aimEnemy;
    task->Reset();

    // Atm we just pick up a target that has best ai weight
    // We multiply it by distance factor since weights are almost not affected by distance.

    Enemy *bestTarget = nullptr;
    float bestScore = 0.0f;

    Vec3 botOrigin(bot->s.origin);
    vec3_t forward, right, up;
    AngleVectors(bot->s.angles, forward, right, up);
    Vec3 botDirection(forward);
    botDirection += right;
    botDirection += up;

    // Until these bounds distance factor scales linearly
    constexpr float distanceBounds = 3500.0f;

    for (unsigned i = 0; i < maxTrackedEnemies; ++i)
    {
        Enemy &enemy = enemies[i];
        if (!enemy.ent)
            continue;
        // Not seen in this frame enemies have zero weight;
        if (!enemy.weight)
            continue;

        Vec3 botToEnemy = botOrigin - enemy.ent->s.origin;
        float distance = botToEnemy.LengthFast();
        botToEnemy *= 1.0f / distance;
        float distanceFactor = 0.3f + 0.7f * BoundedFraction(distance, distanceBounds);
        float directionFactor = 0.7f + 0.3f * botDirection.Dot(botToEnemy);

        float currScore = enemy.weight * distanceFactor * directionFactor;
        if (currScore > bestScore)
        {
            bestScore = currScore;
            bestTarget = &enemy;
        }
    }

    if (bestTarget)
    {
        EnqueueTarget(bestTarget->ent);
        task->aimEnemy = bestTarget;
        task->prevSpamEnemy = nullptr;
        task->instanceId = NextCombatTaskInstanceId();
        nextTargetChoiceAt = level.time + aimTargetChoicePeriod;
        Debug("TryFindNewCombatTask(): found aim enemy %s, next target choice at %09d\n", bestTarget->Nick(), nextTargetChoiceAt);
        SuggestAimWeaponAndTactics(task);
    }
    else
    {
        if (bot->ai->botRef->hasPendingLookAtPoint)
        {
            Debug("TryFindNewCombatTask(): bot is already turning to some look-at-point, defer target assignment\n");
        }
        else if (oldAimEnemy)
        {
            SuggestPointToTurnToWhenEnemyIsLost(oldAimEnemy);
        }
        else
        {
            SuggestSpamTask(task, botOrigin, botDirection);
        }
    }
}

bool BotBrain::SuggestPointToTurnToWhenEnemyIsLost(const Enemy *oldEnemy)
{
    unsigned notSeenDuration = level.time - oldEnemy->LastSeenAt();
    if (notSeenDuration > 500)
        return false;

    Vec3 estimatedPos(oldEnemy->LastSeenPosition());
    Vec3 lastSeenVelocityDir(oldEnemy->LastSeenVelocity());
    float lastSeenSqSpeed = lastSeenVelocityDir.SquaredLength();
    if (lastSeenSqSpeed > 1)
        lastSeenVelocityDir *= Q_RSqrt(lastSeenSqSpeed);
    // Extrapolate last seen position using last seen velocity and not seen duration in seconds
    estimatedPos += (notSeenDuration / 1000.0f) * lastSeenVelocityDir;

    float turnSpeedMultiplier = 0.25f + 0.75f * BotSkill();
    bot->ai->botRef->SetPendingLookAtPoint(estimatedPos, turnSpeedMultiplier, 750);

    return true;
}

void BotBrain::SuggestSpamTask(CombatTask *task, const Vec3 &botOrigin, const Vec3 &botViewDirection)
{
    // Low-skill bots never spam
    if (BotSkill() < 0.33f)
        return;

    // TODO: Do not spam if bot tries to hide
    static_assert(NOT_SEEN_TIMEOUT > 2000, "This value will yield too low spam enemy timeout");
    // If enemy has been not seen more than timeout, do not start spam at this enemy location
    const unsigned timeout = (unsigned)((NOT_SEEN_TIMEOUT - 1000) * BotSkill());

    float bestScore = 0.0f;
    const Enemy *bestEnemy = nullptr;

    for (unsigned i = 0; i < maxTrackedEnemies; ++i)
    {
        const Enemy &enemy = enemies[i];
        if (!enemy.ent)
            continue;
        if (enemy.weight)
            continue;
        if (&enemy == task->prevSpamEnemy)
            continue;

        // Spamming is not affected by distance, it is quite useful vs snipers
        Vec3 botToSpotDirection = enemy.LastSeenPosition() - botOrigin;
        botToSpotDirection.NormalizeFast();
        float directionFactor = 0.3f + 0.7f * botToSpotDirection.Dot(botViewDirection);
        float timeFactor = 1.0f - BoundedFraction(level.time - enemy.LastSeenAt(), timeout);

        float currScore = (0.5f * (enemy.maxPositiveWeight + enemy.avgPositiveWeight)) * directionFactor * timeFactor;
        if (currScore > bestScore)
        {
            bestScore = currScore;
            bestEnemy = &enemy;
        }
    }

    if (bestEnemy)
    {
        StartSpamAtEnemy(task, bestEnemy);
        nextTargetChoiceAt = level.time + spamTargetChoicePeriod;
    }
    else
    {
        // If not set, bot will repeat try to find target on each next frame
        nextTargetChoiceAt = level.time + spamTargetChoicePeriod / 2;
    }

#ifdef _DEBUG
    const char *prevEnemyNick = task->prevSpamEnemy ? task->prevSpamEnemy->Nick() : "<none>";
    const char *bestEnemyNick = bestEnemy ? bestEnemy->Nick() : "<none>";
    const char *format = "SuggestSpamTask(): prev spam enemy %s, chosen (?) one %s, next target choice at %09d\n";
    Debug(format, prevEnemyNick, bestEnemyNick, nextTargetChoiceAt);
#endif

    // TODO: Spam on item spawn points if there are not nearby teammates and item is going to be spawned
    // TODO: Spam on spawn points in non-team-based gametypes
}

void BotBrain::StartSpamAtEnemy(CombatTask *task, const Enemy *enemy)
{
    // Do not add spamTargetChoicePeriod but add very small period when you start spam at enemy (enemy may come back quickly)
    // TODO: Calculate possible enemy position based on last seen ones
    Vec3 spamSpot = enemy->LastSeenPosition();
    task->spamEnemy = enemy;
    task->spamSpot = spamSpot;
    task->instanceId = NextCombatTaskInstanceId();
    SuggestSpamEnemyWeaponAndTactics(task);
    // TODO: Make it dependent of a skill level?
    task->spamTimesOutAt = level.time + 1200;
    unsigned timeDelta = level.time - enemy->LastSeenAt();
    constexpr const char *fmt = "starts spamming at %.3f %.3f %.3f with %s where it has seen %s %d ms ago\n";
    Debug(fmt, spamSpot.x(), spamSpot.y(), spamSpot.z(), WeapName(task->suggestedSpamWeapon), enemy->Nick(), timeDelta);
}

// Old weapon selection code with some style and C to C++ fixes
int BotBrain::SuggestEasyBotsWeapon(const Enemy &enemy)
{
	float best_weight = 0.0;
	int weapon_range = 0, best_weapon = WEAP_NONE;

	const float	dist = DistanceFast(bot->s.origin, enemy.ent->s.origin);

    if (dist < 150)
        weapon_range = AIWEAP_MELEE_RANGE;
    else if (dist < 500)  // Medium range limit is Grenade launcher range
        weapon_range = AIWEAP_SHORT_RANGE;
    else if (dist < 900)
        weapon_range = AIWEAP_MEDIUM_RANGE;
    else
        weapon_range = AIWEAP_LONG_RANGE;

	for (int i = WEAP_GUNBLADE; i < WEAP_TOTAL; i++)
	{
        gsitem_t *weaponItem;
		float rangeWeight;

		if((weaponItem = GS_FindItemByTag(i)) == nullptr)
			continue;

		if(!GS_CheckAmmoInWeapon( &bot->r.client->ps, i ))
			continue;

		rangeWeight = AIWeapons[i].RangeWeight[weapon_range];

		// weigh up if having strong ammo
		if( bot->r.client->ps.inventory[weaponItem->ammo_tag] )
			rangeWeight *= 1.25;

		// add a small random factor (less random the more skill)
		rangeWeight += brandom(-(1.0f - BotSkill()), 1.0f - BotSkill());

		// compare range weights
		if(rangeWeight > best_weight)
		{
			best_weight = rangeWeight;
			best_weapon = i;
		}
	}

    return best_weapon;
}

static constexpr float CLOSE_RANGE = 300.0f;

inline float GetLaserRange()
{
    const auto lgDef = GS_GetWeaponDef(WEAP_LASERGUN);
    return (lgDef->firedef.timeout + lgDef->firedef.timeout) / 2.0f;
}

CombatDisposition BotBrain::GetCombatDisposition(const Enemy &enemy)
{
    float damageToBeKilled = DamageToKill(bot);
    if (BotHasShell())
        damageToBeKilled /= 4.0f;
    if (enemy.HasQuad())
        damageToBeKilled *= 4.0f;
    float damageToKillEnemy = DamageToKill(enemy.ent);
    if (enemy.HasShell())
        damageToKillEnemy *= 4.0f;

    float distance = (enemy.LastSeenPosition() - self->s.origin).LengthFast();

    CombatDisposition disposition;
    disposition.damageToBeKilled = damageToBeKilled;
    disposition.damageToKill = damageToKillEnemy;
    disposition.distance = distance;
    return disposition;
}

void BotBrain::SuggestAimWeaponAndTactics(CombatTask *task)
{
    const Enemy &enemy = *task->aimEnemy;
    if (BotSkill() < 0.33f)
    {
        task->suggestedShootWeapon = SuggestEasyBotsWeapon(enemy);
        return;
    }

    Vec3 botOrigin(bot->s.origin);
    TestTargetEnvironment(botOrigin, enemy.LastSeenPosition(), enemy.ent);

    if (GS_Instagib())
    {
        task->suggestedShootWeapon = SuggestInstagibWeapon(enemy);
        return;
    }

    if (BotHasQuad())
    {
        task->suggestedShootWeapon = SuggestQuadBearerWeapon(enemy);
        return;
    }

    CombatDisposition disposition = GetCombatDisposition(enemy);

    const float lgRange = GetLaserRange();

    if (disposition.distance > lgRange * 2.0f)
        SuggestSniperRangeWeaponAndTactics(task, disposition);
    else if (disposition.distance > lgRange)
        SuggestFarRangeWeaponAndTactics(task, disposition);
    else if (disposition.distance > CLOSE_RANGE)
        SuggestMiddleRangeWeaponAndTactics(task, disposition);
    else
        SuggestCloseRangeWeaponAndTactics(task, disposition);

    if (task->suggestedShootWeapon == WEAP_NONE)
        task->suggestedShootWeapon = WEAP_GUNBLADE;
}

void BotBrain::SuggestSniperRangeWeaponAndTactics(CombatTask *task, const CombatDisposition &disposition)
{
    const Enemy &enemy = *task->aimEnemy;

    int chosenWeapon = WEAP_NONE;

    // Spam plasma from long range to blind enemy
    if (enemy.PendingWeapon() == WEAP_ELECTROBOLT && decisionRandom < BotSkill() / 3.0f)
    {
        if (PlasmasReadyToFireCount())
            chosenWeapon = WEAP_PLASMAGUN;
    }
    if (chosenWeapon == WEAP_NONE)
    {
        if (disposition.damageToKill > 50.0f)
        {
            if (BoltsReadyToFireCount())
                chosenWeapon = WEAP_ELECTROBOLT;
            else if (BulletsReadyToFireCount())
                chosenWeapon = WEAP_MACHINEGUN;
        }
        else
        {
            if (BulletsReadyToFireCount())
                chosenWeapon = WEAP_MACHINEGUN;
            else if (BoltsReadyToFireCount())
                chosenWeapon = WEAP_ELECTROBOLT;
        }
    }
    // Still not chosen
    if (chosenWeapon == WEAP_NONE)
    {
        if (disposition.damageToBeKilled < 25.0f && ShellsReadyToFireCount())
            chosenWeapon = WEAP_RIOTGUN;
        else
        {
            float ratio = disposition.KillToBeKilledDamageRatio();
            if (ratio < 2 || (ratio > 0.75 && decisionRandom < 0.5))
                task->advance = true;
            else
                task->inhibit = true;
        }
    }

    Debug("(sniper range)... : chose %s \n", GS_GetWeaponDef(chosenWeapon)->name);

    task->suggestedShootWeapon = chosenWeapon;
}

struct WeaponAndScore
{
    int weapon;
    float score;
    WeaponAndScore(int weapon = WEAP_NONE, float score = 0.0f)
    {
        this->weapon = weapon;
        this->score = score;
    }
};

int BotBrain::ChooseWeaponByScores(struct WeaponAndScore *begin, struct WeaponAndScore *end)
{
    int weapon = WEAP_NONE;
    const int pendingWeapon = bot->r.client->ps.stats[STAT_PENDING_WEAPON];
    float pendingWeaponScore = std::numeric_limits<float>::infinity();
    for (WeaponAndScore *it = begin; it != end; ++it)
    {
        if (pendingWeapon == it->weapon)
        {
            pendingWeaponScore = it->score + weaponScoreRandom;
            break;
        }
    }
    float maxScore = 0.0f;
    if (pendingWeaponScore != std::numeric_limits<float>::infinity())
    {
        for (WeaponAndScore *it = begin; it != end; ++it)
        {
            float currScore = it->score + weaponScoreRandom;
            if (maxScore < currScore)
            {
                // Do not change weapon if its score is almost equal to current one to avoid weapon choice "jitter"
                // when a bot tries to change weapon infinitely when weapon scores are close to each other
                if (pendingWeapon == it->weapon || fabsf(currScore - pendingWeaponScore) > 0.1f)
                {
                    maxScore = currScore;
                    weapon = it->weapon;
                }
            }
        }
    }
    else
    {
        for (WeaponAndScore *it = begin; it != end; ++it)
        {
            float currScore = it->score + weaponScoreRandom;
            if (maxScore < currScore)
            {
                maxScore = currScore;
                weapon = it->weapon;
            }
        }
    }

    return weapon;
}

void BotBrain::SuggestFarRangeWeaponAndTactics(CombatTask *task, const CombatDisposition &disposition)
{
    const Enemy &enemy = *task->aimEnemy;

    bool botHasMidRangeWeapons = LasersReadyToFireCount() >= 15 ||
        RocketsReadyToFireCount() >= 3 || PlasmasReadyToFireCount() >= 20;
    bool enemyHasMidRangeWeapons = enemy.LasersReadyToFireCount() >= 15 ||
        enemy.RocketsReadyToFireCount() >= 2 || enemy.PlasmasReadyToFireCount() > 15;

    if (botHasMidRangeWeapons && !enemyHasMidRangeWeapons)
        task->advance = true;
    if (!botHasMidRangeWeapons && enemyHasMidRangeWeapons)
        task->retreat = true;

    if (disposition.KillToBeKilledDamageRatio() > 2)
    {
        task->advance = false;
        task->retreat = true;
    }
    else if (disposition.KillToBeKilledDamageRatio() < 1)
    {
        task->advance = true;
        task->retreat = false;
    }

    // First, try to choose a long range weapon of EB, MG, PG and RG
    enum { EB, MG, PG, RG };
    WeaponAndScore weaponScores[4] =
    {
        WeaponAndScore(WEAP_ELECTROBOLT, 1.0f * BoundedFraction(BoltsReadyToFireCount(), 2.0f)),
        WeaponAndScore(WEAP_MACHINEGUN, 1.0f * BoundedFraction(BulletsReadyToFireCount(), 10.0f)),
        WeaponAndScore(WEAP_PLASMAGUN, 0.8f * BoundedFraction(PlasmasReadyToFireCount(), 15.0f)),
        WeaponAndScore(WEAP_RIOTGUN, 0.6f * BoundedFraction(ShellsReadyToFireCount(), 2))
    };

    weaponScores[EB].score += BotSkill() / 3;

    // Counteract EB with PG or MG
    if (enemy.PendingWeapon() == WEAP_ELECTROBOLT)
    {
        weaponScores[PG].score *= 1.3f;
        weaponScores[MG].score *= 1.2f;
    }
    // Counteract PG with MG or EB
    if (enemy.PendingWeapon() == WEAP_PLASMAGUN)
    {
        weaponScores[EB].score *= 1.4f;
        weaponScores[MG].score *= 1.2f;
    }
    // Counteract MG with PG or EB
    if (enemy.PendingWeapon() == WEAP_MACHINEGUN)
    {
        weaponScores[PG].score *= 1.2f;
        weaponScores[EB].score *= 1.3f;
    }

    // Do not use plasma on fast-moving side-to-side enemies
    Vec3 targetMoveDir = Vec3(enemy.ent->velocity);
    float enemySpeed = targetMoveDir.SquaredLength();
    if (enemySpeed > 0.1f)
    {
        enemySpeed = 1.0f / Q_RSqrt(enemySpeed);
    }
    if (enemySpeed > DEFAULT_DASHSPEED)
    {
        targetMoveDir *= 1.0f / enemySpeed;
        Vec3 botToTargetDir = enemy.LastSeenPosition() - bot->s.origin;
        botToTargetDir.NormalizeFast();

        float speedFactor = BoundedFraction(enemySpeed - DEFAULT_DASHSPEED, 1000.0f - DEFAULT_DASHSPEED);
        float dirFactor = fabsf(botToTargetDir.Dot(targetMoveDir));
        // If enemy moves fast but on botToTargetDir line, pg score is unaffected
        weaponScores[PG].score *= 1.0f - (1.0f - dirFactor) * speedFactor;
    }

    int chosenWeapon = ChooseWeaponByScores(weaponScores, weaponScores + 4);

    if (chosenWeapon == WEAP_NONE)
    {
        // Bot needs to have lots of rocket since most of rockets will not hit at this distance
        float rocketScore = targetEnvironment.factor * std::min(6, RocketsReadyToFireCount()) / 6.0f;
        if (rocketScore > 0.4f)
        {
            chosenWeapon = WEAP_ROCKETLAUNCHER;
            if (!task->retreat)
                task->advance = true;
        }
    }
    Debug("(far range)... chose %s\n", GS_GetWeaponDef(chosenWeapon)->name);

    task->suggestedShootWeapon = chosenWeapon;
}

void BotBrain::SuggestMiddleRangeWeaponAndTactics(CombatTask *task, const CombatDisposition &disposition)
{
    const Enemy &enemy = *task->aimEnemy;
    const float distance = disposition.distance;
    const float lgRange = GetLaserRange();
    // Should be equal to max mid range distance - min mid range distance
    const float midRangeLen = lgRange - CLOSE_RANGE;
    // Relative distance from min mid range distance
    const float midRangeDistance = distance - CLOSE_RANGE;

    int chosenWeapon = WEAP_NONE;

    // First, give a bot some apriori tactics based on health status
    if (disposition.KillToBeKilledDamageRatio() < 0.75f)
        task->advance = true;
    else if (disposition.KillToBeKilledDamageRatio() > 1.5f)
        task->retreat = true;

    enum { RL, LG, PG, MG, RG, GL };
    WeaponAndScore weaponScores[6];
    weaponScores[RL].weapon = WEAP_ROCKETLAUNCHER;
    weaponScores[LG].weapon = WEAP_LASERGUN;
    weaponScores[PG].weapon = WEAP_PLASMAGUN;
    weaponScores[MG].weapon = WEAP_MACHINEGUN;
    weaponScores[RG].weapon = WEAP_RIOTGUN;
    weaponScores[GL].weapon = WEAP_GRENADELAUNCHER;

    weaponScores[RL].score = 1.0f * BoundedFraction(RocketsReadyToFireCount(), 3.0f);
    weaponScores[LG].score = 1.0f * BoundedFraction(LasersReadyToFireCount(), 20.0f);
    weaponScores[PG].score = 0.9f * BoundedFraction(PlasmasReadyToFireCount(), 25.0f);
    weaponScores[MG].score = 0.8f * BoundedFraction(BulletsReadyToFireCount(), 20.0f);
    weaponScores[RG].score = 0.7f * BoundedFraction(ShellsReadyToFireCount(), 3.0f);
    weaponScores[GL].score = 0.5f * BoundedFraction(GrenadesReadyToFireCount(), 5.0f);

    if (task->advance)
    {
        weaponScores[RL].score *= 1.3f;
        weaponScores[GL].score *= 0.7f;
        weaponScores[RG].score *= 1.1f;
    }
    if (task->retreat)
    {
        // Plasma is a great defensive weapon
        weaponScores[PG].score *= 1.5f;
        weaponScores[LG].score *= 1.1f;
        weaponScores[MG].score *= 1.1f;
        weaponScores[GL].score *= 1.1f;
    }

    // 1 on mid range bound, 0 on close range bound
    float distanceFactor = (distance - 250.0f) / (lgRange - 250.0f);

    weaponScores[RL].score *= 1.0f - 0.7f * distanceFactor;
    weaponScores[LG].score *= 0.6f + 0.4f * distanceFactor;
    weaponScores[PG].score *= 1.0f - 0.4f * distanceFactor;
    weaponScores[MG].score *= 0.3f + 0.7f * distanceFactor;
    weaponScores[RG].score *= 1.0f - 0.7f * distanceFactor;
    // GL score is maximal in the middle on mid-range zone and is zero on the zone bounds
    weaponScores[GL].score *= 1.0f - fabsf(midRangeDistance - midRangeLen / 2.0f) / midRangeDistance;

    weaponScores[RL].score *= targetEnvironment.factor;
    weaponScores[LG].score *= 1.0f - 0.5f * targetEnvironment.factor;
    weaponScores[PG].score *= 0.5f + 0.5f * targetEnvironment.factor;
    weaponScores[MG].score *= 1.0f - targetEnvironment.factor;
    weaponScores[RG].score *= 1.0f - 0.5f * targetEnvironment.factor;
    weaponScores[GL].score *= targetEnvironment.factor;

    chosenWeapon = ChooseWeaponByScores(weaponScores, weaponScores + 6);

    // Correct tactics aposteriori based on weapons choice
    switch (chosenWeapon)
    {
        case WEAP_ROCKETLAUNCHER:
            if (!task->retreat)
                task->advance = true;
            break;
        case WEAP_LASERGUN:
            // If enemy may hit a bot with mid-range weapons that may affect bot LG accuracy by knockback
            if (enemy.RocketsReadyToFireCount() || enemy.PlasmasReadyToFireCount() || enemy.ShellsReadyToFireCount())
            {
                // Try to stay on a distance that is close to laser length (+ some delta for maneuver), but not more
                if (distance + 125.0f > lgRange)
                    task->advance = true;
                else if (distance + 250.0f < lgRange)
                    task->retreat = true;
            }
            else
            {
                if (!task->retreat)
                    task->advance = true;
            }
            break;
        case WEAP_PLASMAGUN:
            // Come closer to enemy, except enemies having RL in good for RL environment
            if (!enemy.RocketsReadyToFireCount() || targetEnvironment.factor < 0.4)
            {
                if (!task->retreat)
                    task->advance = true;
            }
            break;
        case WEAP_MACHINEGUN:
            // Don't fight with MG versus mid-range weapons
            if (enemy.LasersReadyToFireCount() > 5 || enemy.PlasmasReadyToFireCount() > 5 ||
                enemy.ShellsReadyToFireCount() || enemy.RocketsReadyToFireCount())
            {
                task->retreat = true;
            }
            // Don't come close to enemy except he is sniping
            else if (distance < 1.75f * CLOSE_RANGE)
            {
                if (enemy.PendingWeapon() == WEAP_ELECTROBOLT)
                    task->advance = true;
                else
                    task->retreat = true;
            }
            break;
        case WEAP_RIOTGUN:
            // Don't fight with RG versus stream-like weapons
            if (enemy.LasersReadyToFireCount() > 5 || enemy.PlasmasReadyToFireCount() > 5)
            {
                if (!task->advance)
                    task->retreat = true;
            }
            // Come closer to enemy, except having RL enemy in good for RL environment
            else if (!enemy.RocketsReadyToFireCount() || targetEnvironment.factor < 0.5f)
            {
                if (!task->retreat)
                    task->advance = true;
            }
            break;
        case WEAP_GRENADELAUNCHER:
            // Don't fight with GL versus stream-like weapons
            if (enemy.LasersReadyToFireCount() > 5 || enemy.PlasmasReadyToFireCount() > 5)
                task->retreat = true;
            // Don't come close to enemy except very good for GL environment
            else if (distance < lgRange / 2.0f && targetEnvironment.factor < 0.75f)
                task->retreat = true;
            else if (!task->retreat)
                task->advance = true;
            break;
        default:
            if (BoltsReadyToFireCount())
            {
                chosenWeapon = WEAP_ELECTROBOLT;
                // Retreat except, maybe, case when enemies are weak
                if (!task->advance)
                    task->retreat = true;
            }
            break;
    }

#ifdef _DEBUG
    float rl = weaponScores[RL].score;
    float lg = weaponScores[LG].score;
    float pg = weaponScores[PG].score;
    float mg = weaponScores[MG].score;
    float rg = weaponScores[RG].score;
    float gl = weaponScores[GL].score;
    const char *format = "(mid range) raw scores: RL %1.2f LG %1.2f PG %1.2f MG %1.2f RG %1.2f GL %1.2f chose %s\n";
    Debug(format, rl, lg, pg, mg, rg, gl, GS_GetWeaponDef(chosenWeapon)->name);
#endif

    task->suggestedShootWeapon = chosenWeapon;
}

void BotBrain::SuggestCloseRangeWeaponAndTactics(CombatTask *task, const CombatDisposition &disposition)
{
    int chosenWeapon = WEAP_NONE;

    int lasersCount = LasersReadyToFireCount();
    int rocketsCount = RocketsReadyToFireCount();
    int plasmasCount = PlasmasReadyToFireCount();

    if (g_allow_selfdamage->integer)
    {
        // Prefer lasers over explosives to avoid selfdamage
        if (lasersCount > 10)
            chosenWeapon = WEAP_LASERGUN;
        else if (plasmasCount > 10 && disposition.damageToBeKilled > 50)
        {
            chosenWeapon = WEAP_PLASMAGUN;
            task->retreat = true;
        }
        else if (rocketsCount > 0 && disposition.damageToBeKilled > 80)
        {
            chosenWeapon = WEAP_ROCKETLAUNCHER;
            task->retreat = true;
        }
    }
    else
    {
        if (plasmasCount > 10)
        {
            chosenWeapon = WEAP_PLASMAGUN;
            task->advance = true;
        }
        else if (rocketsCount)
        {
            chosenWeapon = WEAP_ROCKETLAUNCHER;
            task->advance = true;
        }
        else if (lasersCount > 10)
        {
            chosenWeapon = WEAP_LASERGUN;
            // Other players will tend to attack with explosives, so bot has to retreat
            task->retreat = true;
        }
    }
    // Still not chosen
    if (chosenWeapon == WEAP_NONE)
    {
        int shellsCount = ShellsReadyToFireCount();
        if (shellsCount > 0)
            chosenWeapon = WEAP_RIOTGUN;
            // High-skill bots have high accuracy, so they may fight quite well with EB on short range.
        else if (random() < BotSkill() && BoltsReadyToFireCount())
        {
            chosenWeapon = WEAP_ELECTROBOLT;
            task->retreat = true;
        }
        else if (BulletsReadyToFireCount() > 0)
        {
            chosenWeapon = WEAP_MACHINEGUN;
            task->retreat = true;
        }
        else if (lasersCount > 0)
        {
            chosenWeapon = WEAP_LASERGUN;
            task->retreat = true;
        }
        else if (plasmasCount > 0 && disposition.damageToBeKilled > 35)
        {
            chosenWeapon = WEAP_PLASMAGUN;
            task->retreat = true;
        }
        else
        {
            chosenWeapon = WEAP_GUNBLADE;
            if (disposition.KillToBeKilledDamageRatio() < 0.33f)
                task->advance = true;
            else
                task->retreat = true;
        }
    }

    Debug("(close range) : chose %s \n", GS_GetWeaponDef(chosenWeapon)->name);

    task->suggestedShootWeapon = chosenWeapon;
}

int BotBrain::SuggestFinishWeapon(const Enemy &enemy, const CombatDisposition &disposition)
{
    if (disposition.distance < CLOSE_RANGE)
    {
        if (g_allow_selfdamage->integer && disposition.damageToBeKilled < 75)
        {
            if (LasersReadyToFireCount() > disposition.damageToKill * 0.3f * 14)
                return WEAP_LASERGUN;
            if (ShellsReadyToFireCount() > 0)
                return WEAP_RIOTGUN;
            if (BulletsReadyToFireCount() > disposition.damageToKill * 0.3f * 10)
                return WEAP_MACHINEGUN;
            if (PlasmasReadyToFireCount() > disposition.damageToKill * 0.3f * 14)
                return WEAP_PLASMAGUN;
            // Hard bots do not do this high risk action
            if (BotSkill() < 0.66f && RocketsReadyToFireCount())
                return WEAP_ROCKETLAUNCHER;
        }
        else
        {
            if (RocketsReadyToFireCount() && targetEnvironment.factor > 0)
                return WEAP_ROCKETLAUNCHER;
            if (ShellsReadyToFireCount())
                return WEAP_RIOTGUN;
            if (PlasmasReadyToFireCount() > disposition.damageToKill * 0.3f * 14)
                return WEAP_PLASMAGUN;
            if (LasersReadyToFireCount() > disposition.damageToKill * 0.3f * 14)
                return WEAP_LASERGUN;
            if (BulletsReadyToFireCount() > disposition.damageToKill * 0.3f * 10)
                return WEAP_MACHINEGUN;
        }
        return WEAP_GUNBLADE;
    }
    const float lgRange = GetLaserRange();
    if (disposition.distance < lgRange)
    {
        if (disposition.distance < lgRange / 2 && targetEnvironment.factor > 0.6f && RocketsReadyToFireCount())
            return WEAP_ROCKETLAUNCHER;
        if (BoltsReadyToFireCount() && disposition.damageToKill > 30 && enemy.PendingWeapon() == WEAP_LASERGUN)
            return WEAP_ELECTROBOLT;
        if (LasersReadyToFireCount() > disposition.damageToKill * 0.3f * 14)
            return WEAP_LASERGUN;
        if (ShellsReadyToFireCount())
            return WEAP_RIOTGUN;
        if (BulletsReadyToFireCount() > disposition.damageToKill * 0.3f * 10)
            return WEAP_MACHINEGUN;
        if (PlasmasReadyToFireCount() > disposition.damageToKill * 0.3f * 14)
            return WEAP_PLASMAGUN;

        // Surprise...
        if (random() < 0.15f && BotSkill() > 0.5f && (GrenadesReadyToFireCount() && targetEnvironment.factor > 0.5f))
            return WEAP_GRENADELAUNCHER;

        return WEAP_GUNBLADE;
    }

    if (BulletsReadyToFireCount() > disposition.damageToKill * 0.3f * 10)
        return WEAP_MACHINEGUN;
    if (BoltsReadyToFireCount())
        return WEAP_ELECTROBOLT;
    if (ShellsReadyToFireCount())
        return WEAP_RIOTGUN;

    return WEAP_GUNBLADE;
}

static bool IsEscapingFromStandingEntity(const edict_t *escaping, const edict_t *standing, float escapingVelocitySqLen)
{
    // Too low relative speed with almost standing enemy
    if (escaping->speed < DEFAULT_DASHSPEED * 1.35f)
        return false;

    Vec3 escapingVelocityDir(escaping->velocity);
    escapingVelocityDir *= Q_RSqrt(escapingVelocitySqLen);

    Vec3 escapingToStandingDir(standing->s.origin);
    escapingToStandingDir -= escaping->s.origin;

    float len = escapingToStandingDir.SquaredLength();
    if (len < 1)
        return false;

    escapingToStandingDir *= Q_RSqrt(len);
    return escapingToStandingDir.Dot(escapingVelocityDir) < -0.5f;
}

bool BotBrain::IsEnemyEscaping(const Enemy &enemy, const CombatDisposition &disposition)
{
    // Very basic. Todo: Check env. behind an enemy or the bot, is it really tries to escape or just pushed on a wall

    float botVelocitySqLen = VectorLengthSquared(bot->velocity);
    float enemyVelocitySqLen = VectorLengthSquared(enemy.ent->velocity);

    // Enemy is moving fast
    if (enemyVelocitySqLen >= DEFAULT_DASHSPEED * DEFAULT_DASHSPEED)
    {
        // Both entities are moving fast
        if (botVelocitySqLen >= DEFAULT_DASHSPEED * DEFAULT_DASHSPEED)
        {
            Vec3 botVelocityDir(bot->velocity);
            Vec3 enemyVelocityDir(enemy.ent->velocity);
            enemyVelocityDir *= Q_RSqrt(enemyVelocitySqLen);
            botVelocityDir *= Q_RSqrt(botVelocitySqLen);
            return botVelocityDir.Dot(enemyVelocityDir) < -0.5f;
        }
        // Bot is standing or walking, direction of its speed does not matter
        return IsEscapingFromStandingEntity(enemy.ent, bot, enemyVelocitySqLen);
    }

    // Enemy is standing or walking, direction of its speed does not matter
    return IsEscapingFromStandingEntity(enemy.ent, bot, enemyVelocitySqLen);
}

int BotBrain::SuggestHitEscapingEnemyWeapon(const Enemy &enemy, const CombatDisposition &disposition)
{
    const float lgRange = GetLaserRange();

    // We think its too dangerous to switch weapons in this case
    if (disposition.distance < lgRange - 150)
    {
        Debug("(hit escaping) too small distance %.1f to change weapon, too risky\n", disposition.distance);
        return WEAP_NONE;
    }

    enum { EB, RL, GB, MAX_WEAPONS };
    WeaponAndScore weaponScores[MAX_WEAPONS] =
    {
        WeaponAndScore(WEAP_ELECTROBOLT, BoltsReadyToFireCount() > 0),
        WeaponAndScore(WEAP_ROCKETLAUNCHER, RocketsReadyToFireCount() > 0),
        WeaponAndScore(WEAP_GUNBLADE, 0.8f)
    };

    weaponScores[EB].score *= 1.0f + 0.33f * BotSkill();

    weaponScores[RL].score *= 1.33f * (0.3f + 0.7f * targetEnvironment.factor);
    weaponScores[GB].score *= 0.6f + 0.4f * targetEnvironment.factor;

    weaponScores[EB].score *= 0.3f + 0.7f * BoundedFraction(disposition.distance, 2000.0f);
    weaponScores[RL].score *= 1.0f - BoundedFraction(disposition.distance, 1500.0f);
    weaponScores[GB].score *= 1.0f - 0.3f * BoundedFraction(disposition.distance, 2500.0f);


    // We are sure that weapon switch not only costs nothing, but even is intended, so do not call ChooseWeaponByScore()
    int weapon = WEAP_NONE;
    float maxScore = 0.0f;
    for (int i = 0; i < MAX_WEAPONS; ++i)
    {
        if (maxScore < weaponScores[i].score)
        {
            maxScore = weaponScores[i].score;
            weapon = weaponScores[i].weapon;
        }
    }

    constexpr const char *format = "(hit escaping) raw scores: EB %.2f RL %.2f GB %.2f chose %s\n";
    Debug(format, weaponScores[EB].score, weaponScores[RL].score, weaponScores[GB].score, WeapName(weapon));

    return weapon;
}

bool BotBrain::CheckForShotOfDespair(const Enemy &enemy, const CombatDisposition &disposition)
{
    if (BotHasPowerups())
        return false;

    // Restrict weapon switch time even more compared to generic fast switch action
    if (self->r.client->ps.stats[STAT_WEAPON_TIME] > 16)
        return false;

    float adjustedDamageToBeKilled = disposition.damageToBeKilled * (enemy.HasQuad() ? 0.25f : 1.0f);
    if (adjustedDamageToBeKilled > 25)
        return false;

    if (disposition.damageToKill < 35)
        return false;

    const float lgRange = GetLaserRange();

    if (disposition.distance > lgRange)
        return false;

    switch (enemy.PendingWeapon())
    {
        case WEAP_LASERGUN:
            return true;
        case WEAP_PLASMAGUN:
            return random() > disposition.distance / lgRange;
        case WEAP_ROCKETLAUNCHER:
            return random() > disposition.distance / lgRange;
        case WEAP_MACHINEGUN:
            return true;
        default:
            return false;
    }
}

int BotBrain::SuggestShotOfDespairWeapon(const Enemy &enemy, const CombatDisposition &disposition)
{
    // Prevent negative scores from self-damage suicide.
    int score = self->r.client->ps.stats[STAT_SCORE];
    if (level.gametype.inverseScore)
        score *= -1;

    if (score <= 0)
    {
        if (BoltsReadyToFireCount() > 0)
            return WEAP_ELECTROBOLT;
        if (ShellsReadyToFireCount() > 0)
            return WEAP_RIOTGUN;
        return WEAP_NONE;
    }

    const float lgRange = GetLaserRange();

    enum { EB, RG, RL, GB, GL, WEIGHTS_COUNT };

    WeaponAndScore scores[WEIGHTS_COUNT] =
    {
        WeaponAndScore(WEAP_ELECTROBOLT, BoltsReadyToFireCount() > 0),
        WeaponAndScore(WEAP_RIOTGUN, ShellsReadyToFireCount() > 0),
        WeaponAndScore(WEAP_ROCKETLAUNCHER, RocketsReadyToFireCount() > 0),
        WeaponAndScore(WEAP_GUNBLADE, 0.8f),
        WeaponAndScore(WEAP_GRENADELAUNCHER, 0.7f)
    };

    TestTargetEnvironment(Vec3(self->s.origin), Vec3(enemy.ent->s.origin), enemy.ent);

    // Do not touch hitscan weapons scores, we are not going to do a continuous fight
    scores[RL].score *= targetEnvironment.factor;
    scores[GL].score *= targetEnvironment.factor;
    scores[GB].score *= 0.5f + 0.5f * targetEnvironment.factor;

    // Since shots of despair are done in LG range, do not touch GB
    scores[RL].score *= 1.0f - 0.750f * disposition.distance / lgRange;
    scores[GL].score *= 1.0f - 0.999f * disposition.distance / lgRange;

    // Add extra scores for very close shots (we are not going to prevent bot suicide)
    if (disposition.distance < 150)
    {
        scores[RL].score *= 2.0f;
        scores[GL].score *= 2.0f;
    }

    // Prioritize EB for relatively far shots
    if (disposition.distance > lgRange * 0.66)
        scores[EB].score *= 1.5f;

    // Counteract some weapons with their antipodes
    switch (enemy.PendingWeapon())
    {
        case WEAP_LASERGUN:
        case WEAP_PLASMAGUN:
            scores[RL].score *= 1.75f;
            scores[GL].score *= 1.35f;
            break;
        case WEAP_ROCKETLAUNCHER:
            scores[RG].score *= 2.0f;
            break;
        default: // Shut up inspections
            break;
    }

    // Do not call ChoseWeaponByScores() which gives current weapon a slight priority, weapon switch is intended
    float bestScore = 0;
    int bestWeapon = WEAP_NONE;
    for (const auto &weaponAndScore: scores)
    {
        if (bestScore < weaponAndScore.score)
        {
            bestScore = weaponAndScore.score;
            bestWeapon = weaponAndScore.weapon;
        }
    }

    return bestWeapon;
}

int BotBrain::SuggestQuadBearerWeapon(const Enemy &enemy)
{
    float distance = (enemy.LastSeenPosition() - bot->s.origin).LengthFast();
    auto lgDef = GS_GetWeaponDef(WEAP_LASERGUN);
    auto lgRange = (lgDef->firedef.timeout + lgDef->firedef_weak.timeout) / 2.0f;
    int lasersCount = 0;
    if (Inventory()[WEAP_LASERGUN] && distance < lgRange)
    {
        if ((lgDef->firedef.timeout + lgDef->firedef_weak.timeout) / 2 < distance)
        {
            lasersCount = Inventory()[AMMO_LASERS] + Inventory()[AMMO_WEAK_LASERS];
            if (lasersCount > 7)
                return WEAP_LASERGUN;
        }
    }
    int bulletsCount = BulletsReadyToFireCount();
    if (bulletsCount > 10)
        return WEAP_MACHINEGUN;
    int plasmasCount = PlasmasReadyToFireCount();
    if (plasmasCount > 10)
        return WEAP_PLASMAGUN;
    if (ShellsReadyToFireCount())
        return WEAP_RIOTGUN;
    if (Inventory()[WEAP_ROCKETLAUNCHER])
    {
        if (RocketsReadyToFireCount() > 0 && distance > CLOSE_RANGE && distance < lgRange * 1.25f)
            return WEAP_ROCKETLAUNCHER;
    }
    if (lasersCount > 0 && distance < lgRange)
        return WEAP_LASERGUN;
    if (bulletsCount > 0)
        return WEAP_MACHINEGUN;
    if (plasmasCount > 0)
        return WEAP_PLASMAGUN;
    if (Inventory()[WEAP_GRENADELAUNCHER])
    {
        if (GrenadesReadyToFireCount() > 0 && distance > CLOSE_RANGE && distance < lgRange)
        {
            float deltaZ = bot->s.origin[2] - enemy.LastSeenPosition().z();
            if (deltaZ < -250.0f && random() > 0.5f)
                return WEAP_GRENADELAUNCHER;
        }
    }
    return WEAP_GUNBLADE;
}

int BotBrain::SuggestInstagibWeapon(const Enemy &enemy)
{
    // Prefer hitscan weapons
    if (BulletsReadyToFireCount())
        return WEAP_MACHINEGUN;
    if (Inventory()[WEAP_LASERGUN])
    {
        auto lgDef = GS_GetWeaponDef(WEAP_LASERGUN);
        float squaredDistance = (enemy.LastSeenPosition() - bot->s.origin).SquaredLength();
        if (Inventory()[AMMO_LASERS] && squaredDistance < lgDef->firedef.timeout * lgDef->firedef.timeout)
            return WEAP_LASERGUN;
        if (Inventory()[AMMO_WEAK_LASERS] && squaredDistance < lgDef->firedef_weak.timeout * lgDef->firedef_weak.timeout)
            return WEAP_LASERGUN;
    }
    if (ShellsReadyToFireCount())
        return WEAP_RIOTGUN;

    if (Inventory()[WEAP_PLASMAGUN])
    {
        float squaredDistance = (enemy.LastSeenPosition() - bot->s.origin).SquaredLength();
        if (squaredDistance < 1000 && PlasmasReadyToFireCount())
            return WEAP_PLASMAGUN;
    }
    if (Inventory()[WEAP_INSTAGUN])
        return WEAP_INSTAGUN;
    if (BoltsReadyToFireCount())
        return WEAP_ELECTROBOLT;
    return WEAP_GUNBLADE;
}

void BotBrain::SuggestSpamEnemyWeaponAndTactics(CombatTask *task)
{
    const Enemy &enemy = *task->spamEnemy;

#ifdef _DEBUG
    {
        const Vec3 &position = enemy.LastSeenPosition();
        constexpr const char *format = "SuggestSpamWeapon...(): %s has been last seen at %f %f %f\n";
        Debug(format, enemy.Nick(), position.x(), position.y(), position.z());
    }
#endif
    Vec3 botToSpotVec = enemy.LastSeenPosition();
    botToSpotVec -= bot->s.origin;
    float distance = botToSpotVec.LengthFast();

    TestTargetEnvironment(Vec3(bot->s.origin), enemy.LastSeenPosition(), nullptr);

    enum { PG, MG, RL, GL, GB, WEIGHTS_COUNT };
    const float distanceBounds[WEIGHTS_COUNT] = { 5000.0f, 2000.0f, 1500.0f, 1100.0f, 3000.0f };
    float distanceFactors[WEIGHTS_COUNT];
    for (int i = 0; i < WEIGHTS_COUNT; ++i)
    {
        distanceFactors[i] = BoundedFraction(distance, distanceBounds[i]);
    }

    WeaponAndScore scores[5] =
    {
        WeaponAndScore(WEAP_PLASMAGUN, 1.0f * BoundedFraction(PlasmasReadyToFireCount(), 30)),
        WeaponAndScore(WEAP_MACHINEGUN, 0.7f * BoundedFraction(BulletsReadyToFireCount(), 30)),
        WeaponAndScore(WEAP_ROCKETLAUNCHER, 1.2f * BoundedFraction(RocketsReadyToFireCount(), 5)),
        WeaponAndScore(WEAP_GRENADELAUNCHER, 1.0f * BoundedFraction(GrenadesReadyToFireCount(), 3)),
        WeaponAndScore(WEAP_GUNBLADE, 0.5f)
    };

    // All scores decrease linearly with distance (except for MG)
    scores[PG].score *= 1.0f - 0.8f * distanceFactors[PG];
    scores[MG].score *= distanceFactors[MG];
    scores[RL].score *= 0.5f - 0.5f * distanceFactors[RL];
    scores[GL].score *= 1.0f - distanceFactors[GL];
    scores[GB].score *= 1.0f - 0.5f * distanceFactors[GB];

    scores[PG].score *= 0.7f + 0.3f * targetEnvironment.factor;
    scores[MG].score *= 1.0f - targetEnvironment.factor;
    scores[RL].score *= 0.2f + 0.8f * targetEnvironment.factor;
    scores[GL].score *= targetEnvironment.factor;
    scores[GB].score *= 0.4f + 0.6 * targetEnvironment.factor;

    float deltaZ = bot->s.origin[2] - enemy.LastSeenPosition().z();
    float deltaZBound = g_gravity->value / 2;
    // 0 for enemies that are deltaZBound or more units higher, 1 for enemies that are deltaZBound or more lower
    float gravityFactor = (std::min(deltaZ, deltaZBound) / deltaZBound + 1.0f) / 2.0f;
    scores[GL].score *= 1.5f * gravityFactor;

    // Don't hit itself
    if (distance < 200.0f)
        scores[RL].score = 0;
    if (distance < 250.0f)
        scores[GB].score = 0;
    if (distance < 50.0f)
        scores[PG].score = 0;
    if (distance < 70.0f)
        scores[GB].score = 0;

    int weapon = ChooseWeaponByScores(scores, scores + 5);

    // Do not spam if there are no spam weapons and the enemy is close, choose a weapon to be ready to fight
    if (weapon <= WEAP_GUNBLADE && level.time - enemy.LastSeenAt() < 1000)
    {
        task->inhibit = true;

        if (distance < GetLaserRange())
        {
            if (LasersReadyToFireCount() > 15)
                weapon = WEAP_LASERGUN;
            else if (ShellsReadyToFireCount() > 0)
                weapon = WEAP_RIOTGUN;
        }
        else if (BoltsReadyToFireCount() > 0)
            weapon = WEAP_ELECTROBOLT;
        else if (PlasmasReadyToFireCount() > 0)
            weapon = WEAP_PLASMAGUN;
    }

    constexpr const char *fmt = "spam raw weapon scores: PG %.2f MG %.2f RL %.2f GL %.2f GB %.2f chose %s\n";
    Debug(fmt, scores[PG].score, scores[MG].score, scores[RL].score, scores[GL].score, scores[GB].score, WeapName(weapon));

    task->suggestedSpamWeapon = weapon;
}

const float BotBrain::TargetEnvironment::TRACE_DEPTH = 250.0f;

void BotBrain::TestTargetEnvironment(const Vec3 &botOrigin, const Vec3 &targetOrigin, const edict_t *traceKey)
{
    Vec3 forward = targetOrigin - botOrigin;
    forward.z() = 0;
    float frontSquareLen = forward.SquaredLength();
    if (frontSquareLen > 1)
    {
        // Normalize
        forward *= Q_RSqrt(frontSquareLen);
    }
    else
    {
        // Pick dummy horizontal direction
        forward = Vec3(&axis_identity[AXIS_FORWARD]);
    }
    Vec3 right = Vec3(&axis_identity[AXIS_UP]).Cross(forward);

    // Now botToTarget is a normalized horizontal part of botToTarget - botOrigin

    edict_t *passedict = const_cast<edict_t*>(traceKey);
    float *start = const_cast<float*>(botOrigin.data());
    trace_t *traces = targetEnvironment.sideTraces;

    const float TRACE_DEPTH = TargetEnvironment::TRACE_DEPTH;

    vec_t offsets[6 * 3];
    VectorSet(offsets + 3 * TargetEnvironment::TOP, 0, 0, +TRACE_DEPTH);
    VectorSet(offsets + 3 * TargetEnvironment::BOTTOM, 0, 0, -TRACE_DEPTH);
    VectorSet(offsets + 3 * TargetEnvironment::FRONT, -TRACE_DEPTH, 0, 0);
    VectorSet(offsets + 3 * TargetEnvironment::BACK, +TRACE_DEPTH, 0, 0);
    VectorSet(offsets + 3 * TargetEnvironment::LEFT, 0, -TRACE_DEPTH, 0);
    VectorSet(offsets + 3 * TargetEnvironment::RIGHT, 0, +TRACE_DEPTH, 0);

    vec3_t mins = { -32, -32, -32 };
    vec3_t maxs = { +32, +32, +32 };

    float factor = 0.0f;
    for (int i = 0; i < 6; ++i)
    {
        vec3_t end;
        trace_t *trace = traces + i;
        G_ProjectSource(start, offsets + 3 * i, forward.data(), right.data(), end);
        G_Trace(trace, start, mins, maxs, end, passedict, MASK_AISOLID);
        // Give some non-zero score by the fact that trace is detected a hit itself
        if (trace->fraction < 1.0f)
            factor += 1.0f / 6.0f;
        // Compute a dot product between a bot-to-target direction and trace-point-to-target direction
        // If the dot product is close to 1, a bot may shoot almost perpendicular to the traced point.
        // If trace point is itself close to a target, bot may inflict enemy lots of damage by rockets.
        Vec3 hitPointToTarget = targetOrigin - trace->endpos;
        if (trace->fraction > 0.01f)
        {
            // Normalize
            hitPointToTarget *= 1.0f / (TRACE_DEPTH * trace->fraction);
            factor += 1.5f * (1.0f - trace->fraction) * fabsf(hitPointToTarget.Dot(forward));
        }
        else
        {
            // The target is very close to a traced solid surface anyway, use 1 instead of a dot product.
            factor += 1.5f * (1.0f - trace->fraction);
        }
    }

    targetEnvironment.factor = std::min(1.0f, factor / 6.0f);
}

void BotBrain::UpdatePotentialGoalsWeights()
{
    // Compute it once, not on each loop step
    bool onlyGotGB = true;
    for (int weapon = WEAP_GUNBLADE + 1; weapon < WEAP_TOTAL; ++weapon)
    {
        if (Inventory()[weapon])
        {
            onlyGotGB = false;
            break;
        }
    }

    FOREACH_GOALENT(goalEnt)
    {
        entityWeights[goalEnt->Id()] = 0;

        // item timing disabled by now
        if (!goalEnt->IsSpawnedAtm())
            continue;

        // Picking clients as goal entities is currently disabled
        if (goalEnt->IsClient())
            continue;

        if (goalEnt->Item())
            entityWeights[goalEnt->Id()] = ComputeItemWeight(goalEnt->Item(), onlyGotGB);
    }
}

float BotBrain::ComputeItemWeight(const gsitem_t *item, bool onlyGotGB) const
{
    switch (item->type)
    {
        case IT_WEAPON: return ComputeWeaponWeight(item, onlyGotGB);
        case IT_AMMO: return ComputeAmmoWeight(item);
        case IT_HEALTH: return ComputeHealthWeight(item);
        case IT_ARMOR: return ComputeArmorWeight(item);
        case IT_POWERUP: return ComputePowerupWeight(item);
    }
    return 0;
}

float BotBrain::ComputeWeaponWeight(const gsitem_t *item, bool onlyGotGB) const
{
    if (Inventory()[item->tag])
    {
        // TODO: Precache
        const gsitem_t *ammo = GS_FindItemByTag(item->ammo_tag);
        if (Inventory()[ammo->tag] >= ammo->inventory_max)
            return 0;

        float ammoQuantityFactor = 1.0f - Inventory()[ammo->tag] / (float)ammo->inventory_max;

        switch (item->tag)
        {
            case WEAP_ELECTROBOLT: return ammoQuantityFactor;
            case WEAP_LASERGUN: return ammoQuantityFactor * 1.1f;
            case WEAP_PLASMAGUN: return ammoQuantityFactor * 1.1f;
            case WEAP_ROCKETLAUNCHER: return ammoQuantityFactor;
            default: return 0.5f * ammoQuantityFactor;
        }
    }

    // We may consider plasmagun in a bot's hand as a top tier weapon too
    const int topTierWeapons[4] = { WEAP_ELECTROBOLT, WEAP_LASERGUN, WEAP_ROCKETLAUNCHER, WEAP_PLASMAGUN };

    // TODO: Precompute
    float topTierWeaponGreed = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        if (!Inventory()[topTierWeapons[i]])
            topTierWeaponGreed += 1.0f;
    }

    for (int i = 0; i < 4; ++i)
    {
        if (topTierWeapons[i] == item->tag)
            return (onlyGotGB ? 1.5f : 0.9f) + (topTierWeaponGreed - 1.0f) / 3.0f;
    }

    return onlyGotGB ? 1.5f : 0.7f;
}

float BotBrain::ComputeAmmoWeight(const gsitem_t *item) const
{
    if (Inventory()[item->tag] < item->inventory_max)
    {
        float quantityFactor = 1.0f - Inventory()[item->tag] / (float)item->inventory_max;

        for (int weapon = WEAP_GUNBLADE; weapon < WEAP_TOTAL; weapon++)
        {
            // TODO: Preache
            const gsitem_t *weaponItem = GS_FindItemByTag( weapon );
            if (weaponItem->ammo_tag == item->tag)
            {
                if (Inventory()[weaponItem->tag])
                {
                    switch (weaponItem->tag)
                    {
                        case WEAP_ELECTROBOLT: return quantityFactor;
                        case WEAP_LASERGUN: return quantityFactor * 1.1f;
                        case WEAP_PLASMAGUN: return quantityFactor * 1.1f;
                        case WEAP_ROCKETLAUNCHER: return quantityFactor;
                        default: return 0.5f * quantityFactor;
                    }
                }
                return quantityFactor * 0.33f;
            }
        }
    }
    return 0.0;
}

float BotBrain::ComputeHealthWeight(const gsitem_t *item) const
{
    if (item->tag == HEALTH_MEGA || item->tag == HEALTH_ULTRA)
        return 2.5f;

    if (item->tag == HEALTH_SMALL)
        return 0.2f + 0.3f * (1.0f - self->health / (float)self->max_health);

    return std::max(0.0f, 1.0f - self->health / (float)self->max_health);
}

float BotBrain::ComputeArmorWeight(const gsitem_t *item) const
{
    float currArmor = bot->r.client->resp.armor;
    switch (item->tag)
    {
        case ARMOR_RA:
            return currArmor < 150.0f ? 2.0f : 0.0f;
        case ARMOR_YA:
            return currArmor < 125.0f ? 1.7f : 0.0f;
        case ARMOR_GA:
            return currArmor < 100.0f ? 1.4f : 0.0f;
        case ARMOR_SHARD:
        {
            if (currArmor < 25 || currArmor >= 150)
                return 0.4f;
            return 0.25f;
        }
    }
    return 0;
}

float BotBrain::ComputePowerupWeight(const gsitem_t *item) const
{
    // TODO: Make it dependent of current health/armor status;
    return 3.5f;
}