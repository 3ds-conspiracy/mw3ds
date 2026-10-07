package com.github.mw3ds.item;

import org.joml.Vector2f;

import java.util.HashMap;
import java.util.Map;

/**
 * Base item class for the game.
 */
public class Item {

    private String name;
    private boolean stolen;
    private boolean keyItem;
    private Vector2f position;
    private static final Map<String, Item> registry = new HashMap<>();

    public Item(String name, boolean stolen, boolean keyItem) {
        this.name = name;
        this.stolen = stolen;
        this.keyItem = keyItem;
    }

    public static Item getItem(String name) {
        Item item = registry.get(name);
        if (item == null) {
            // Default to non-stolen, non-key item
            item = new Item(name, false, false);
            registry.put(name, item);
        }
        return item;
    }

    public static void registerItem(Item item) {
        registry.put(item.getName(), item);
    }

    public String getName() {
        return name;
    }

    public boolean isStolen() {
        return stolen;
    }

    public void setStolen(boolean stolen) {
        this.stolen = stolen;
    }

    public boolean isKeyItem() {
        return keyItem;
    }

    public Vector2f getPosition() {
        return position;
    }

    public void setPosition(Vector2f position) {
        this.position = position;
    }
}
