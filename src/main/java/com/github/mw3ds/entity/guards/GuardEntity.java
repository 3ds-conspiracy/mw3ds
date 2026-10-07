package com.github.mw3ds.entity.guards;

import com.github.mw3ds.entity.Entity;
import com.github.mw3ds.entity.PlayerEntity;
import com.github.mw3ds.item.Item;
import com.github.mw3ds.world.World;
import com.github.mw3ds.scenario.ScenarioManager;
import com.github.mw3ds.scenario.IntroScenario;

import org.joml.Vector2f;

/**
 * Guard entity that can confiscate stolen items.
 * In the intro scene, guards should only confiscate items,
 * not jail or attack the player.
 */
public class GuardEntity extends Entity {

    private boolean isIntroGuard = false;
    private ScenarioManager scenarioManager;

    public GuardEntity(World world, Vector2f position, ScenarioManager scenarioManager) {
        super(world, position);
        this.scenarioManager = scenarioManager;
    }

    /**
     * Marks this guard as being in the intro scene.
     * Intro guards have restricted behavior - they only confiscate.
     */
    public void setIntroMode(boolean intro) {
        this.isIntroGuard = intro;
    }

    public boolean isIntroMode() {
        return isIntroGuard;
    }

    /**
     * Confiscates a stolen item from the player.
     * This is the ONLY punishment in the intro scene.
     * No jail, no arrest resistance, no combat.
     *
     * @param player the player who was stealing
     * @param item the stolen item to confiscate
     */
    public void confiscateItem(PlayerEntity player, Item item) {
        if (player.getInventory().removeItem(item)) {
            // Item successfully removed from player
            // Guard keeps the item (removed from world)
            notifyStealingDetected(player, item);
        }
    }

    /**
     * Handles detection of stealing behavior.
     * In intro mode, only calls confiscate - never triggers arrest.
     */
    public void onStealingDetected(PlayerEntity player) {
        if (isIntroGuard) {
            // Only confiscate in intro - do NOT jail or initiate combat
            Item stolenItem = findStolenItem(player);
            if (stolenItem != null) {
                confiscateItem(player, stolenItem);
            }
            return;
        }

        // Normal behavior for non-intro scenes
        initiateArrestSequence(player);
    }

    /**
     * Finds the first stolen item in the player's inventory.
     */
    private Item findStolenItem(PlayerEntity player) {
        for (Item item : player.getInventory().getItems()) {
            if (item.isStolen()) {
                return item;
            }
        }
        return null;
    }

    /**
     * Initiates the full arrest sequence (used in non-intro scenes).
     */
    private void initiateArrestSequence(PlayerEntity player) {
        // This is the normal behavior - can lead to jail or combat
        // Only called when NOT in intro mode
        if (scenarioManager != null && scenarioManager.getCurrentScenario() instanceof IntroScenario) {
            // Should not reach here due to isIntroGuard check, but safety first
            return;
        }
        // Normal arrest logic...
    }

    private void notifyStealingDetected(PlayerEntity player, Item item) {
        // Notify the game that stealing was detected
        // In intro, this is purely cosmetic - no actual penalty beyond confiscation
    }

    @Override
    public void update(float delta) {
        super.update(delta);
        // Guard AI - patrol and detect stealing
        updateAI(delta);
    }

    private void updateAI(float delta) {
        // Basic patrol behavior
        // Detect player stealing and respond appropriately
    }
}
